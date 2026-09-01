#ifndef AEGIS_SKILL_REGISTRY_H
#define AEGIS_SKILL_REGISTRY_H
#include "aegis/skill/skill.h"
#include "aegis/types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct aegis_skill_registry aegis_skill_registry_t;
aegis_status_t                      aegis_skill_registry_create(aegis_skill_registry_t** out);
void                                aegis_skill_registry_destroy(aegis_skill_registry_t* reg);
aegis_status_t       aegis_skill_registry_add(aegis_skill_registry_t* reg, aegis_skill_t* skill);
size_t               aegis_skill_registry_count(const aegis_skill_registry_t* reg);
const aegis_skill_t* aegis_skill_registry_get(const aegis_skill_registry_t* reg, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
