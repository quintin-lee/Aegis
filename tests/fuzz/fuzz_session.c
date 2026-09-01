#define _POSIX_C_SOURCE 200809L
#include "aegis/session/session.h"
#include "aegis/message/message.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

// Simple fuzz: feed random bytes to session save/load and tool args
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 10) return 0;
    char proj[64];
    snprintf(proj, sizeof(proj), "/tmp/%.*s", (int)(size % 32), (const char*)data);
    aegis_session_t* s = NULL;
    if (aegis_session_create(proj, &s) != AEGIS_OK) return 0;
    aegis_message_t* m = NULL;
    aegis_message_create(AEGIS_MESSAGE_USER, &m);
    char content[256];
    size_t clen = size % 200;
    memcpy(content, data, clen);
    content[clen] = '\0';
    for (size_t i = 0; i < clen; i++) if ((unsigned char)content[i] < 32 && content[i] != '\n') content[i] = ' ';
    aegis_message_set_content(m, content);
    aegis_session_append_message(s, m);
    aegis_message_destroy(m);
    char path[] = "/tmp/fuzz_sess_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) {
        close(fd);
        aegis_session_save(s, path);
        aegis_session_t* loaded = NULL;
        aegis_session_load(path, &loaded);
        if (loaded) aegis_session_destroy(loaded);
        unlink(path);
    }
    aegis_session_destroy(s);
    return 0;
}

#ifdef FUZZ_STANDALONE
int main(void) {
    const char* samples[] = {
        "{\"type\":\"message\",\"role\":\"user\",\"content\":\"hello\"}",
        "not json at all \xFF\xFE",
        "{\"type\":\"session_start\",\"id\":\"123\"}",
        ""
    };
    for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); i++) {
        LLVMFuzzerTestOneInput((const uint8_t*)samples[i], strlen(samples[i]));
    }
    printf("fuzz standalone PASS\n");
    return 0;
}
#endif
