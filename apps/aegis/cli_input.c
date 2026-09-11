/**
 * @file cli_input.c
 * @brief Interactive stdin plumbing: background reader thread, line queue,
 * raw terminal mode (Esc-to-interrupt). Lines typed mid-turn are queued,
 * never lost; an empty line means interrupt. PTY quit works via shutdown
 * flag since a PTY never delivers EOF while attached.
 */
#define _POSIX_C_SOURCE 200809L
#include "cli_repl.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* ── stdin reader thread + line queue ────────────────────────────────────
 * A background thread reads stdin into a FIFO so lines typed during a
 * running turn are not lost: an empty line interrupts the turn, a
 * non-empty line queues as the next input. */

void lq_init(line_queue_t* q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
}

void lq_push(line_queue_t* q, char* text)
{
    line_cell_t* c = malloc(sizeof(*c));
    if (!c) {
        free(text);
        return;
    }
    *c = (line_cell_t){.text = text, .next = NULL};
    pthread_mutex_lock(&q->mu);
    if (q->tail) {
        q->tail->next = c;
    } else {
        q->head = c;
    }
    q->tail = c;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

/** Blocking pop; returns NULL on EOF. */
char* lq_pop(line_queue_t* q)
{
    pthread_mutex_lock(&q->mu);
    while (!q->head && !q->closed) {
        pthread_cond_wait(&q->cv, &q->mu);
    }
    line_cell_t* c    = q->head;
    char*        text = NULL;
    if (c) {
        q->head = c->next;
        if (!q->head) {
            q->tail = NULL;
        }
        text = c->text;
        free(c);
    }
    pthread_mutex_unlock(&q->mu);
    return text;
}

void lq_close(line_queue_t* q)
{
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

line_queue_t g_lines;

/* Set on quit: releases the poll-waiting reader even without EOF (a PTY
 * never delivers EOF while the session stays attached). */
volatile sig_atomic_t g_reader_shutdown = 0;

/* ── Raw-mode input (Esc-to-interrupt) ────────────────────────────────────
 * A real interactive terminal is switched into non-canonical mode so a lone
 * Esc byte arrives immediately (Claude-Code-style "press Esc to stop").
 * ICANON | ECHO are cleared, ISIG is kept (Ctrl-C still terminates). When
 * stdin is not a TTY (tests, pipes) none of this applies. */
struct termios g_orig_tio;
bool           g_raw_enabled = false;

void raw_enable(void)
{
    if (!isatty(STDIN_FILENO)) {
        return;
    }
    if (tcgetattr(STDIN_FILENO, &g_orig_tio) != 0) {
        return;
    }
    struct termios raw = g_orig_tio;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
        g_raw_enabled = true;
    }
}

void raw_disable(void)
{
    if (g_raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_tio);
        g_raw_enabled = false;
    }
}

/** Raw-mode echo of a single erase (backspace) step. */
static void raw_erase(size_t* n)
{
    if (*n > 0) {
        (*n)--;
        fputs("\b \b", stdout);
        fflush(stdout);
    }
}

/* Read one line from stdin with a timeout so shutdown can release us.
 * Returns 1 with *line set (NUL-terminated, no newline), 0 on timeout/shutdown,
 * -1 on EOF.
 *
 * Raw mode (TTY only): a lone Esc (0x1b) returns 1 with an EMPTY line — the
 * caller routes empty lines to interrupt, exactly like Enter-to-interrupt.
 * Esc + CSI/SS3 introducer ('[' or 'O') is consumed as an ignored arrow/function
 * sequence. Printing chars are echoed; backspace erases. */
static int read_line_timeout(char* buf, size_t cap)
{
    size_t n = 0;
    for (;;) {
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
        int           rc  = poll(&pfd, 1, 100);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            if (g_reader_shutdown) {
                return 0;
            }
            continue;
        }
        char    ch;
        ssize_t got = read(STDIN_FILENO, &ch, 1);
        if (got == 0) {
            return n > 0 ? (buf[n] = '\0', 1) : -1;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (g_raw_enabled) {
            if (ch == 0x1b) {
                /* Lone Esc → interrupt (empty line). A following '[' or 'O'
                 * within a short window marks an escape sequence to ignore. */
                struct pollfd seq = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
                if (poll(&seq, 1, 30) <= 0) {
                    buf[0] = '\0';
                    return 1;
                }
                /* Consume until the final byte of the CSI/SS3 sequence. */
                char c2;
                if (read(STDIN_FILENO, &c2, 1) == 1 && (c2 == '[' || c2 == 'O')) {
                    char c3;
                    for (;;) {
                        if (read(STDIN_FILENO, &c3, 1) != 1) {
                            break;
                        }
                        if (c3 >= 0x40 && c3 <= 0x7e) {
                            break; /* final byte */
                        }
                    }
                }
                continue; /* ignored sequence; keep assembling the line */
            }
            if (ch == '\n' || ch == '\r') {
                buf[n] = '\0';
                fputs("\r\n", stdout);
                fflush(stdout);
                return 1;
            }
            if (ch == 0x7f || ch == 0x08) {
                raw_erase(&n);
                continue;
            }
            if (n + 1 < cap) {
                buf[n++] = ch;
                fputc(ch, stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            buf[n] = '\0';
            return 1;
        }
        if (n + 1 < cap) {
            buf[n++] = ch;
        }
    }
}

void* reader_main(void* arg)
{
    (void)arg;
    char buf[4096];
    while (!g_reader_shutdown) {
        int rc = read_line_timeout(buf, sizeof(buf));
        if (rc < 0) {
            break; /* EOF */
        }
        if (rc == 0) {
            continue; /* timed out; re-check shutdown flag */
        }
        lq_push(&g_lines, strdup(buf));
    }
    lq_push(&g_lines, NULL); /* EOF sentinel */
    return NULL;
}
