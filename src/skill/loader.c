#define _POSIX_C_SOURCE 200809L
#include "aegis/skill/loader.h"
#include "aegis/skill/skill.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

aegis_status_t aegis_skill_loader_load_dir(aegis_skill_registry_t* reg, const char* dir)
{
    if (!reg || !dir) {
        return AEGIS_ERR_INVALID;
    }
    DIR* d = opendir(dir);
    if (!d) {
        return AEGIS_OK;  // no dir is not error
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        // Look for SKILL.md or skill.md
        char skill_file[1024];
        snprintf(skill_file, sizeof(skill_file), "%s/SKILL.md", path);
        FILE* f = fopen(skill_file, "r");
        if (!f) {
            snprintf(skill_file, sizeof(skill_file), "%s/skill.md", path);
            f = fopen(skill_file, "r");
            if (!f) {
                continue;
            }
        }
        char desc[1024]  = "";
        char instr[8192] = "";
        // Simple: first line is name, second is desc, rest is instructions
        if (fgets(desc, sizeof(desc), f)) {
            // strip newline
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
        aegis_skill_t* s    = NULL;
        const char*    name = ent->d_name;
        aegis_skill_create(name, desc[0] ? desc : NULL, instr[0] ? instr : NULL, &s);
        if (s) {
            s->path = strdup(path);
            aegis_skill_registry_add(reg, s);
        }
    }
    closedir(d);
    return AEGIS_OK;
}
