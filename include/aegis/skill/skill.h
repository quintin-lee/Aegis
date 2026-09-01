#ifndef AEGIS_SKILL_H
#define AEGIS_SKILL_H

#include "aegis/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file skill.h
 * @brief Skill — prompt/instruction capability.
 */

typedef struct aegis_skill {
    char* name;
    char* description;
    char* instructions;
    char* path;
} aegis_skill_t;

aegis_status_t aegis_skill_create(const char* name, const char* desc, const char* instructions,
                                  aegis_skill_t** out);
void           aegis_skill_destroy(aegis_skill_t* skill);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_SKILL_H */
