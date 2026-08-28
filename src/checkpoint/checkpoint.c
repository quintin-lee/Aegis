/**
 * @file checkpoint.c
 * @brief Agent state persistence: save/restore with versioning and integrity.
 */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include "aegis/checkpoint/checkpoint.h"
#include "aegis/status.h"
#include "aegis/executor/cancellation.h"
#include "aegis/agent/agent.h"
#include "aegis/task/task.h"
#include "aegis/task/graph.h"
#include "aegis/planner/plan.h"

#include "checkpoint_internal.h"
#include "lifecycle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* ── CRC32 ─────────────────────────────────────────────────────────────────── */

static uint32_t       crc32_table[256];
static pthread_once_t crc32_once = PTHREAD_ONCE_INIT;

static void crc32_init_table_once(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1u)));
        }
        crc32_table[i] = crc;
    }
}

static uint32_t crc32_compute(const uint8_t* data, size_t len, uint32_t crc)
{
    pthread_once(&crc32_once, crc32_init_table_once);
    crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static const char* agent_state_name(aegis_agent_state_t s)
{
    static const char* names[] = {"CREATED",    "INITIALIZING", "READY",  "RUNNING",   "PAUSED",
                                  "CANCELLING", "COMPLETED",    "FAILED", "CANCELLED", "ABORTED"};
    size_t             idx     = (size_t)s;
    if (idx >= sizeof(names) / sizeof(names[0])) {
        return "UNKNOWN";
    }
    return names[idx];
}

static const char* task_state_name(aegis_task_state_t s)
{
    static const char* names[] = {"PENDING", "READY",  "RUNNING",   "WAITING",
                                  "SUCCESS", "FAILED", "CANCELLED", "SKIPPED"};
    size_t             idx     = (size_t)s;
    if (idx >= sizeof(names) / sizeof(names[0])) {
        return "UNKNOWN";
    }
    return names[idx];
}

/* ── Create / Destroy ──────────────────────────────────────────────────────── */

aegis_status_t aegis_checkpoint_create(aegis_checkpoint_t** out)
{
    AEGIS_CHECK_OUT(out);
    aegis_checkpoint_t* ckpt = calloc(1, sizeof(*ckpt));
    if (!ckpt) {
        return AEGIS_ERR_NOMEM;
    }
    ckpt->version   = 0;
    ckpt->timestamp = (uint64_t)time(NULL);
    *out            = ckpt;
    return AEGIS_OK;
}

void aegis_checkpoint_destroy(aegis_checkpoint_t* ckpt)
{
    if (!ckpt) {
        return;
    }
    free(ckpt->goal);
    free(ckpt->plan_text);
    free(ckpt);
}

/* ── Populate ──────────────────────────────────────────────────────────────── */

aegis_status_t aegis_checkpoint_populate(aegis_checkpoint_t* ckpt, const aegis_agent_t* agent,
                                         const aegis_plan_t* plan, const aegis_task_graph_t* graph,
                                         uint32_t version)
{
    if (!ckpt) {
        return AEGIS_ERR_INVALID;
    }

    if (agent) {
        snprintf(ckpt->agent_state, sizeof(ckpt->agent_state), "%s",
                 agent_state_name(aegis_agent_state(agent)));
    } else {
        snprintf(ckpt->agent_state, sizeof(ckpt->agent_state), "CREATED");
    }

    free(ckpt->goal);
    ckpt->goal = NULL;
    if (agent) {
        const char* goal = aegis_agent_get_goal(agent);
        if (goal) {
            ckpt->goal = strdup(goal);
            if (!ckpt->goal) {
                return AEGIS_ERR_NOMEM;
            }
        }
    }

    free(ckpt->plan_text);
    ckpt->plan_text    = NULL;
    ckpt->plan_version = 0;
    if (plan) {
        ckpt->plan_version = aegis_plan_version(plan);
        char* serialized   = NULL;
        if (aegis_plan_serialize(plan, &serialized) == AEGIS_OK && serialized) {
            ckpt->plan_text = serialized;
        }
    }

    ckpt->n_tasks = 0;
    if (graph) {
        aegis_task_t** tasks = NULL;
        size_t         count = 0;
        if (aegis_task_graph_tasks(graph, &tasks, &count) == AEGIS_OK && tasks) {
            size_t limit = count < AEGIS_CHECKPOINT_MAX_TASKS ? count : AEGIS_CHECKPOINT_MAX_TASKS;
            for (size_t i = 0; i < limit; i++) {
                aegis_task_t* t = tasks[i];
                if (!t) {
                    continue;
                }
                aegis_checkpoint_task_snapshot_t* snap = &ckpt->tasks[ckpt->n_tasks];
                snap->task_id                          = aegis_task_id(t);
                strncpy(snap->task_name, aegis_task_name(t), sizeof(snap->task_name) - 1);
                snap->task_name[sizeof(snap->task_name) - 1] = '\0';
                snap->task_state                             = (int)aegis_task_state(t);
                const char* err                              = aegis_task_error(t);
                if (err) {
                    strncpy(snap->error_msg, err, sizeof(snap->error_msg) - 1);
                    snap->error_msg[sizeof(snap->error_msg) - 1] = '\0';
                } else {
                    snap->error_msg[0] = '\0';
                }
                aegis_task_retry_policy_t rp = aegis_task_retry_policy(t);
                snap->retry_count            = 0;
                snap->max_retries            = rp.max_attempts;
                ckpt->n_tasks++;
            }
            free(tasks);
        }
    }

    if (version > 0) {
        ckpt->version = version;
    } else if (ckpt->version == 0) {
        ckpt->version = 1;
    }
    ckpt->timestamp = (uint64_t)time(NULL);
    return AEGIS_OK;
}

aegis_status_t aegis_checkpoint_set_goal(aegis_checkpoint_t* ckpt, const char* goal)
{
    if (!ckpt) {
        return AEGIS_ERR_INVALID;
    }
    free(ckpt->goal);
    ckpt->goal = NULL;
    if (goal) {
        ckpt->goal = strdup(goal);
        if (!ckpt->goal) {
            return AEGIS_ERR_NOMEM;
        }
    }
    return AEGIS_OK;
}

/* ── Accessors ─────────────────────────────────────────────────────────────── */

uint32_t aegis_checkpoint_version(const aegis_checkpoint_t* ckpt)
{
    return ckpt ? ckpt->version : 0;
}

uint64_t aegis_checkpoint_timestamp(const aegis_checkpoint_t* ckpt)
{
    return ckpt ? ckpt->timestamp : 0;
}

const char* aegis_checkpoint_goal(const aegis_checkpoint_t* ckpt)
{
    return ckpt ? (ckpt->goal ? ckpt->goal : "") : "";
}

uint32_t aegis_checkpoint_plan_version(const aegis_checkpoint_t* ckpt)
{
    return ckpt ? ckpt->plan_version : 0;
}

const char* aegis_checkpoint_plan_text(const aegis_checkpoint_t* ckpt)
{
    return ckpt ? ckpt->plan_text : NULL;
}

const char* aegis_checkpoint_agent_state(const aegis_checkpoint_t* ckpt)
{
    return ckpt ? ckpt->agent_state : "CREATED";
}

size_t aegis_checkpoint_task_count(const aegis_checkpoint_t* ckpt)
{
    return ckpt ? ckpt->n_tasks : 0;
}

const aegis_checkpoint_task_snapshot_t* aegis_checkpoint_task_snapshot(
    const aegis_checkpoint_t* ckpt, size_t idx)
{
    if (!ckpt || idx >= ckpt->n_tasks) {
        return NULL;
    }
    return &ckpt->tasks[idx];
}

/* ── Serialization ─────────────────────────────────────────────────────────── */

aegis_status_t aegis_checkpoint_serialize(const aegis_checkpoint_t* ckpt, char** out)
{
    if (!ckpt || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;

    size_t est = 256 + strlen(ckpt->agent_state) + (ckpt->goal ? strlen(ckpt->goal) : 0) +
                 (ckpt->plan_text ? strlen(ckpt->plan_text) + 20 : 0);
    for (size_t i = 0; i < ckpt->n_tasks; i++) {
        est += 128;
    }

    char* buf = malloc(est);
    if (!buf) {
        return AEGIS_ERR_NOMEM;
    }

    int n = snprintf(buf, est,
                     AEGIS_CHECKPOINT_MAGIC
                     " v%u\n"
                     "# TS=%lu\n"
                     "# AGENT_STATE=%s\n"
                     "# GOAL=%s\n"
                     "# PLAN_VERSION=%u\n",
                     ckpt->version, (unsigned long)ckpt->timestamp, ckpt->agent_state,
                     ckpt->goal ? ckpt->goal : "", ckpt->plan_version);
    if (n < 0 || (size_t)n >= est) {
        free(buf);
        return AEGIS_ERR_NOMEM;
    }
    size_t pos = (size_t)n;

    if (ckpt->plan_text) {
        n = snprintf(buf + pos, est - pos, "PLAN_START\n%s\nPLAN_END\n", ckpt->plan_text);
    } else {
        n = snprintf(buf + pos, est - pos, "PLAN_START\n\nPLAN_END\n");
    }
    if (n < 0 || (size_t)(pos + n) >= est) {
        free(buf);
        return AEGIS_ERR_NOMEM;
    }
    pos += (size_t)n;

    for (size_t i = 0; i < ckpt->n_tasks; i++) {
        const aegis_checkpoint_task_snapshot_t* t = &ckpt->tasks[i];
        n = snprintf(buf + pos, est - pos, "TASK id=%u name=%s state=%s error=%s retries=%d/%d\n",
                     t->task_id, t->task_name, task_state_name((aegis_task_state_t)t->task_state),
                     t->error_msg, t->retry_count, t->max_retries);
        if (n < 0 || (size_t)(pos + n) >= est) {
            free(buf);
            return AEGIS_ERR_NOMEM;
        }
        pos += (size_t)n;
    }

    uint32_t crc = crc32_compute((const uint8_t*)buf, pos, 0);
    n            = snprintf(buf + pos, est - pos, "# CRC32=%08x\n", crc);
    if (n < 0 || (size_t)(pos + n) >= est) {
        free(buf);
        return AEGIS_ERR_NOMEM;
    }

    *out = buf;
    return AEGIS_OK;
}

/* ── Deserialization ───────────────────────────────────────────────────────── */

static char* read_file_to_string(const char* path)
{
    FILE* fp = fopen(path, "r");
    if (!fp) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        return NULL;
    }
    char* data = malloc((size_t)size + 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    size_t read_len = fread(data, 1, (size_t)size, fp);
    fclose(fp);
    data[read_len] = '\0';
    return data;
}

aegis_status_t aegis_checkpoint_deserialize(const char* text, aegis_checkpoint_t** out)
{
    if (!text || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;

    aegis_checkpoint_t* ckpt = calloc(1, sizeof(*ckpt));
    if (!ckpt) {
        return AEGIS_ERR_NOMEM;
    }

    /* Parse version from first line: "AEGISCHK v<N>". */
    if (strncmp(text, AEGIS_CHECKPOINT_MAGIC, AEGIS_CHECKPOINT_MAGIC_LEN) == 0) {
        const char* p = text + AEGIS_CHECKPOINT_MAGIC_LEN;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == 'v') {
            p++;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            ckpt->version = (uint32_t)strtoul(p, NULL, 10);
        }
    }

    /* Find first newline to skip the version line. */
    const char* first_newline = strchr(text, '\n');
    if (!first_newline) {
        free(ckpt);
        return AEGIS_ERR_INVALID;
    }
    const char* p = first_newline + 1;

    /* Parse header lines starting with "# ". */
    while (*p) {
        if (p[0] == '#' && (p[1] == ' ' || p[1] == '\n')) {
            p += 2;
            const char* line_start = p;
            const char* line_end   = strchr(p, '\n');
            size_t      line_len   = line_end ? (size_t)(line_end - p) : strlen(p);

            if (strncmp(p, "TS=", 3) == 0) {
                ckpt->timestamp = (uint64_t)strtoull(p + 3, NULL, 10);
            } else if (strncmp(p, "AGENT_STATE=", 12) == 0) {
                size_t vlen = line_len - 12;
                if (vlen >= sizeof(ckpt->agent_state)) {
                    vlen = sizeof(ckpt->agent_state) - 1;
                }
                memcpy(ckpt->agent_state, p + 12, vlen);
                ckpt->agent_state[vlen] = '\0';
            } else if (strncmp(p, "GOAL=", 5) == 0) {
                size_t vlen = line_len - 5;
                ckpt->goal  = malloc(vlen + 1);
                if (ckpt->goal) {
                    memcpy(ckpt->goal, p + 5, vlen);
                    ckpt->goal[vlen] = '\0';
                }
            } else if (strncmp(p, "PLAN_VERSION=", 13) == 0) {
                ckpt->plan_version = (uint32_t)strtoul(p + 13, NULL, 10);
            }
            p = line_end ? line_end + 1 : line_start + line_len;
        } else {
            p++;
        }
    }

    /* Parse PLAN_START/END block. */
    const char* plan_start = strstr(text, "PLAN_START\n");
    const char* plan_end   = strstr(text, "\nPLAN_END");
    if (plan_start && plan_end && plan_end > plan_start) {
        size_t len      = (size_t)(plan_end - plan_start - 10);
        ckpt->plan_text = malloc(len + 1);
        if (ckpt->plan_text) {
            memcpy(ckpt->plan_text, plan_start + 10, len);
            ckpt->plan_text[len] = '\0';
        }
    }

    /* Parse TASK lines. */
    p = text;
    while ((p = strstr(p, "TASK ")) != NULL && ckpt->n_tasks < AEGIS_CHECKPOINT_MAX_TASKS) {
        const char* line_end = strchr(p, '\n');
        size_t      line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        if (line_len >= 512) {
            p = line_end ? line_end + 1 : p + line_len;
            continue;
        }
        char line_buf[512];
        memcpy(line_buf, p, line_len);
        line_buf[line_len] = '\0';
        p                  = line_end ? line_end + 1 : p + line_len;

        aegis_checkpoint_task_snapshot_t* t = &ckpt->tasks[ckpt->n_tasks];
        memset(t, 0, sizeof(*t));

        char* tok = line_buf;
        while (*tok) {
            while (*tok == ' ') {
                tok++;
            }
            if (!*tok) {
                break;
            }

            if (strncmp(tok, "id=", 3) == 0) {
                tok += 3;
                t->task_id = (uint32_t)strtoul(tok, NULL, 10);
                while (*tok && *tok != ' ') {
                    tok++;
                }
            } else if (strncmp(tok, "name=", 5) == 0) {
                tok += 5;
                size_t nlen = 0;
                while (tok[nlen] && tok[nlen] != ' ' && nlen < sizeof(t->task_name) - 1) {
                    nlen++;
                }
                memcpy(t->task_name, tok, nlen);
                t->task_name[nlen] = '\0';
                tok += nlen;
            } else if (strncmp(tok, "state=", 6) == 0) {
                tok += 6;
                t->task_state = (int)strtoul(tok, NULL, 10);
                while (*tok && *tok != ' ') {
                    tok++;
                }
            } else if (strncmp(tok, "error=", 6) == 0) {
                tok += 6;
                size_t elen = 0;
                while (tok[elen] && tok[elen] != ' ' && elen < sizeof(t->error_msg) - 1) {
                    elen++;
                }
                memcpy(t->error_msg, tok, elen);
                t->error_msg[elen] = '\0';
                tok += elen;
            } else if (strncmp(tok, "retries=", 8) == 0) {
                tok += 8;
                t->retry_count = (int)strtoul(tok, NULL, 10);
                while (*tok && *tok != '/') {
                    tok++;
                }
                if (*tok == '/') {
                    tok++;
                }
                t->max_retries = (int)strtoul(tok, NULL, 10);
                while (*tok && *tok != ' ') {
                    tok++;
                }
            } else {
                while (*tok && *tok != ' ') {
                    tok++;
                }
            }
        }
        ckpt->n_tasks++;
    }

    *out = ckpt;
    return AEGIS_OK;
}

/* ── Atomic write ──────────────────────────────────────────────────────────── */

aegis_status_t aegis_checkpoint_write(const aegis_checkpoint_t* ckpt, const char* path,
                                      const aegis_cancellation_token_t* token)
{
    if (!ckpt || !path) {
        return AEGIS_ERR_INVALID;
    }
    if (token && aegis_cancellation_token_is_cancelled(token)) {
        return AEGIS_ERR_CANCELLED;
    }

    char*          serialized = NULL;
    aegis_status_t rc         = aegis_checkpoint_serialize(ckpt, &serialized);
    if (rc != AEGIS_OK) {
        return rc;
    }

    char tmp_path[1024];
    int  n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%u", path, (unsigned)getpid());
    if (n < 0 || n >= (int)sizeof(tmp_path)) {
        free(serialized);
        return AEGIS_ERR_INVALID;
    }

    FILE* fp = fopen(tmp_path, "w");
    if (!fp) {
        free(serialized);
        return AEGIS_ERR_INTERNAL;
    }

    size_t written = fwrite(serialized, 1, strlen(serialized), fp);
    if (fflush(fp) != 0 || written != strlen(serialized)) {
        fclose(fp);
        unlink(tmp_path);
        free(serialized);
        return AEGIS_ERR_INTERNAL;
    }
    fclose(fp);
    free(serialized);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return AEGIS_ERR_INTERNAL;
    }
    return AEGIS_OK;
}

/* ── Restore ───────────────────────────────────────────────────────────────── */

aegis_status_t aegis_checkpoint_read(const char* path, aegis_checkpoint_t** out,
                                     aegis_checkpoint_status_t* status)
{
    if (!path || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;
    if (status) {
        *status = AEGIS_CHECKPOINT_OK;
    }

    char* data = read_file_to_string(path);
    if (!data) {
        if (status) {
            *status = (errno == ENOENT) ? AEGIS_CHECKPOINT_MISSING : AEGIS_CHECKPOINT_CORRUPTED;
        }
        return AEGIS_ERR_NOT_FOUND;
    }

    size_t data_len = strlen(data);
    if (data_len < AEGIS_CHECKPOINT_MAGIC_LEN ||
        memcmp(data, AEGIS_CHECKPOINT_MAGIC, AEGIS_CHECKPOINT_MAGIC_LEN) != 0) {
        free(data);
        if (status) {
            *status = AEGIS_CHECKPOINT_CORRUPTED;
        }
        return AEGIS_ERR_INVALID;
    }

    char* crc_marker = strstr(data, "# CRC32=");
    if (crc_marker) {
        size_t       crc_data_len = (size_t)(crc_marker - data);
        uint32_t     computed     = crc32_compute((const uint8_t*)data, crc_data_len, 0);
        unsigned int stored       = 0;
        if (sscanf(crc_marker + 8, "%08x", &stored) != 1) {
            stored = 0;
        }
        if (computed != stored) {
            free(data);
            if (status) {
                *status = AEGIS_CHECKPOINT_CORRUPTED;
            }
            return AEGIS_ERR_INVALID;
        }
    }

    char* ver_marker = strstr(data, AEGIS_CHECKPOINT_MAGIC " v");
    if (ver_marker) {
        uint32_t file_version =
            (uint32_t)strtoul(ver_marker + AEGIS_CHECKPOINT_MAGIC_LEN + 2, NULL, 10);
        if (file_version != AEGIS_CHECKPOINT_ABI_VERSION) {
            free(data);
            if (status) {
                *status = AEGIS_CHECKPOINT_VERSION_MISMATCH;
            }
            return AEGIS_ERR_INVALID;
        }
    }

    aegis_status_t rc = aegis_checkpoint_deserialize(data, out);
    free(data);
    if (rc == AEGIS_OK && status) {
        *status = AEGIS_CHECKPOINT_OK;
    }
    return rc;
}

const char* aegis_checkpoint_status_str(aegis_checkpoint_status_t status)
{
    switch (status) {
    case AEGIS_CHECKPOINT_OK:
        return "OK";
    case AEGIS_CHECKPOINT_MISSING:
        return "MISSING";
    case AEGIS_CHECKPOINT_CORRUPTED:
        return "CORRUPTED";
    case AEGIS_CHECKPOINT_INCOMPLETE:
        return "INCOMPLETE";
    case AEGIS_CHECKPOINT_VERSION_MISMATCH:
        return "VERSION_MISMATCH";
    default:
        return "UNKNOWN";
    }
}
