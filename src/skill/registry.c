/**
 * @file registry.c
 * @brief Vector-backed ordered skill registry. Add transfers ownership;
 * get returns borrowed pointers; destroy frees all entries. Not thread-safe.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/skill/registry.h"
#include "aegis/common/vector.h"
#include <stdlib.h>

struct aegis_skill_registry {
    aegis_vector_t* vec;
};

/**
 * @brief Create an empty skill registry.
 */
aegis_status_t aegis_skill_registry_create(aegis_skill_registry_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    aegis_skill_registry_t* r = (aegis_skill_registry_t*)calloc(1, sizeof(*r));
    if (!r) {
        return AEGIS_ERR_NOMEM;
    }
    if (aegis_vector_create(&r->vec, sizeof(aegis_skill_t*)) != 0) {
        free(r);
        return AEGIS_ERR_NOMEM;
    }
    *out = r;
    return AEGIS_OK;
}

/**
 * @brief Destroy a skill registry, freeing all owned skills. NULL-safe.
 */
void aegis_skill_registry_destroy(aegis_skill_registry_t* r)
{
    if (!r) {
        return;
    }
    size_t n = aegis_vector_len(r->vec);
    for (size_t i = 0; i < n; i++) {
        aegis_skill_t* s = NULL;
        aegis_vector_get(r->vec, i, &s);
        aegis_skill_destroy(s);
    }
    aegis_vector_destroy(r->vec);
    free(r);
}

/**
 * @brief Add a skill to the registry, transferring ownership.
 *
 * @param[in] r  Registry (must be non-NULL).
 * @param[in] s  Skill to add (ownership transferred; not destroyed on error).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for NULL args,
 *         AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_skill_registry_add(aegis_skill_registry_t* r, aegis_skill_t* s)
{
    if (!r || !s) {
        return AEGIS_ERR_INVALID;
    }
    if (aegis_vector_push(r->vec, &s) != 0) {
        return AEGIS_ERR_NOMEM;
    }
    return AEGIS_OK;
}

/**
 * @brief Return the number of skills in the registry.
 */
size_t aegis_skill_registry_count(const aegis_skill_registry_t* r)
{
    return r ? aegis_vector_len(r->vec) : 0;
}

/**
 * @brief Get a borrowed skill pointer at the given index.
 *
 * @param[in] r    Registry.
 * @param[in] idx  Zero-based index.
 * @return A non-NULL borrowed skill on success, NULL if out of range or
 *         the registry is NULL.
 */
const aegis_skill_t* aegis_skill_registry_get(const aegis_skill_registry_t* r, size_t idx)
{
    if (!r) {
        return NULL;
    }
    aegis_skill_t* s = NULL;
    if (aegis_vector_get(r->vec, idx, &s) != 0) {
        return NULL;
    }
    return s;
}
