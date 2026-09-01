#ifndef AEGIS_SKILL_LOADER_H
#define AEGIS_SKILL_LOADER_H
#include "aegis/skill/registry.h"
#ifdef __cplusplus
extern "C" {
#endif
aegis_status_t aegis_skill_loader_load_dir(aegis_skill_registry_t* reg, const char* dir);
#ifdef __cplusplus
}
#endif
#endif
