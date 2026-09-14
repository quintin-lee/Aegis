/**
 * @file loader.c
 * @brief Skill directory scan: each subdirectory with SKILL.md becomes
 * a registry entry via the manifest parser. Missing directories and
 * skill-less subdirectories are skipped (best-effort, never fatal);
 * registry-add failures free the skill instead of leaking it.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/skill/loader.h"
#include "aegis/skill/manifest.h"
#include "aegis/skill/skill.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/**
 * @brief Scan a directory for subdirectories containing SKILL.md and add
 * each to the registry.
 *
 * Missing or empty directories return AEGIS_OK (best-effort scan).
 * Subdirectories without a manifest are silently skipped.
 *
 * @param[in] reg  Target skill registry.
 * @param[in] dir  Directory to scan for skill subdirectories.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args.
 */
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
        struct stat stbuf;
        if (stat(path, &stbuf) != 0 || !S_ISDIR(stbuf.st_mode)) {
            continue;
        }
        aegis_skill_t* s   = NULL;
        aegis_status_t pst = aegis_skill_manifest_parse(path, &s);
        if (pst == AEGIS_OK && s) {
            if (aegis_skill_registry_add(reg, s) != AEGIS_OK) {
                aegis_skill_destroy(s);
            }
        }
    }
    closedir(d);
    return AEGIS_OK;
}
