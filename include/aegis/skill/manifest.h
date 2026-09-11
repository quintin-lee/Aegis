#ifndef AEGIS_SKILL_MANIFEST_H
#define AEGIS_SKILL_MANIFEST_H
#include "aegis/skill/skill.h"
#include "aegis/types.h"
#ifdef __cplusplus
extern "C" {
#endif
/**
 * @file manifest.h
 * @brief On-disk skill manifest format and parser.
 *
 * A skill is a directory containing SKILL.md (or lowercase skill.md):
 *   - the directory basename is the skill name (required, non-empty),
 *   - the first line of SKILL.md is the one-line description (may be empty),
 *   - the remaining lines are the instructions body (may be empty).
 *
 * The parser validates the layout and returns a fully populated skill
 * (including path); callers transfer it into a registry or destroy it.
 * Directories without a readable SKILL.md report AEGIS_ERR_NOT_FOUND so
 * directory scans can skip them without failing.
 */
aegis_status_t aegis_skill_manifest_parse(const char* dir_path, aegis_skill_t** out);
#ifdef __cplusplus
}
#endif
#endif /* AEGIS_SKILL_MANIFEST_H */
