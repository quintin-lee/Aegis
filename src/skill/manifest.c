/**
 * @file manifest.c
 * @brief On-disk skill manifest parser.
 *
 * Layout: directory basename is the skill name; SKILL.md (or lowercase
 * skill.md) holds the one-line description first, instructions after.
 * Missing/unreadable manifests report NOT_FOUND so directory scans skip
 * them; malformed paths report INVALID. Mirrors the historical loader
 * behavior byte-for-byte, only factored out and validated.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/skill/manifest.h"
#include "aegis/skill/skill.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

aegis_status_t aegis_skill_manifest_parse(const char* dir_path, aegis_skill_t** out)
{
    if (!dir_path || !out) {
        return AEGIS_ERR_INVALID;
    }
    *out = NULL;
    const char* base = strrchr(dir_path, '/');
    base             = base ? base + 1 : dir_path;
    if (base[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    char  skill_file[2048];
    FILE* f = NULL;
    snprintf(skill_file, sizeof(skill_file), "%s/SKILL.md", dir_path);
    f = fopen(skill_file, "r");
    if (!f) {
        snprintf(skill_file, sizeof(skill_file), "%s/skill.md", dir_path);
        f = fopen(skill_file, "r");
        if (!f) {
            return AEGIS_ERR_NOT_FOUND;
        }
    }
    char desc[1024]  = "";
    char instr[8192] = "";
    if (fgets(desc, sizeof(desc), f)) {
        desc[strcspn(desc, "\r\n")] = 0;
    }
    size_t off = 0;
    char   line[1024];
    while (fgets(line, sizeof(line), f) && off < sizeof(instr) - 1) {
        size_t len = strlen(line);
        if (off + len < sizeof(instr)) {
            memcpy(instr + off, line, len);
            off += len;
        }
    }
    fclose(f);
    aegis_skill_t* s  = NULL;
    aegis_status_t st = aegis_skill_create(base, desc[0] ? desc : NULL,
                                           instr[0] ? instr : NULL, &s);
    if (st != AEGIS_OK) {
        return st;
    }
    s->path = strdup(dir_path);
    if (!s->path) {
        aegis_skill_destroy(s);
        return AEGIS_ERR_NOMEM;
    }
    *out = s;
    return AEGIS_OK;
}
