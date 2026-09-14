/**
 * @file skill.c
 * @brief Skill value object: heap-owned name/description/instructions/path
 * with copying create and NULL-safe destroy.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/skill/skill.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Create a skill value object with copying semantics.
 *
 * @param[in]  name   Skill name (required, copied).
 * @param[in]  desc   Optional description (may be NULL).
 * @param[in]  instr  Optional instructions (may be NULL).
 * @param[out] out    Receives the new skill.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID if name is NULL,
 *         AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_skill_create(const char* name, const char* desc, const char* instr,
                                  aegis_skill_t** out)
{
    if (!name || !out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_skill_t* s = (aegis_skill_t*)calloc(1, sizeof(*s));
    if (!s) {
        return AEGIS_ERR_NOMEM;
    }
    s->name = strdup(name);
    if (desc) {
        s->description = strdup(desc);
    }
    if (instr) {
        s->instructions = strdup(instr);
    }
    if (!s->name) {
        aegis_skill_destroy(s);
        return AEGIS_ERR_NOMEM;
    }
    *out = s;
    return AEGIS_OK;
}

/**
 * @brief Destroy a skill and free its owned strings. NULL-safe.
 */
void aegis_skill_destroy(aegis_skill_t* s)
{
    if (!s) {
        return;
    }
    free(s->name);
    free(s->description);
    free(s->instructions);
    free(s->path);
    free(s);
}
