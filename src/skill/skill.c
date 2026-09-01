#define _POSIX_C_SOURCE 200809L
#include "aegis/skill/skill.h"
#include <stdlib.h>
#include <string.h>

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
