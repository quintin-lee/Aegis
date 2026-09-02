#ifndef AEGIS_CODING_PATH_SAFETY_H
#define AEGIS_CODING_PATH_SAFETY_H

#include <stdbool.h>

/**
 * @file path_safety.h
 * @brief Module-internal path containment shared by coding tools.
 *
 * Rejects absolute paths and any ".." path component so tool file access
 * stays inside the project root. Header-only; included by coding_tools.c
 * and discovery_tools.c only.
 */
static inline bool aegis_safe_relative_path(const char* path)
{
    if (!path || path[0] == '/' || path[0] == '\0') {
        return false;
    }
    const char* p = path;
    while (*p) {
        if ((p == path || p[-1] == '/') && p[0] == '.' && p[1] == '.' &&
            (p[2] == '\0' || p[2] == '/')) {
            return false;
        }
        ++p;
    }
    return true;
}

#endif /* AEGIS_CODING_PATH_SAFETY_H */
