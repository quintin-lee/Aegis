/**
 * @file test_skill_loader.c
 * @brief Unit tests for skill directory loading and manifest parsing/
 * validation against fixture skill trees.
 */
#define _POSIX_C_SOURCE 200809L
#include "aegis/skill/loader.h"
#include "aegis/skill/manifest.h"
#include "aegis/skill/registry.h"
#include "aegis/skill/skill.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char* path, const char* content)
{
    FILE* f = fopen(path, "w");
    assert(f != NULL);
    assert(fputs(content, f) >= 0);
    assert(fclose(f) == 0);
}

static void make_skill_dir(const char* base, const char* name, const char* content)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", base, name);
    assert(mkdir(dir, 0700) == 0);
    if (content) {
        char file[2048];
        snprintf(file, sizeof(file), "%s/SKILL.md", dir);
        write_file(file, content);
    }
}

static void remove_skill_dir(const char* base, const char* name, int has_file)
{
    char file[2048];
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", base, name);
    if (has_file) {
        snprintf(file, sizeof(file), "%s/SKILL.md", dir);
        assert(unlink(file) == 0);
    }
    assert(rmdir(dir) == 0);
}

int main(void)
{
    char tmp[] = "/tmp/skilltestXXXXXX";
    assert(mkdtemp(tmp) != NULL);

    /* Valid skill: dirname is the name, first line the description. */
    make_skill_dir(tmp, "review", "Reviews code.\nCheck style and tests.\n");
    /* Subdir without SKILL.md is skipped; plain files are skipped. */
    make_skill_dir(tmp, "empty", NULL);
    {
        char stray[1024];
        snprintf(stray, sizeof(stray), "%s/notes.txt", tmp);
        write_file(stray, "not a skill");
    }

    aegis_skill_registry_t* reg = NULL;
    assert(aegis_skill_registry_create(&reg) == AEGIS_OK);
    assert(aegis_skill_loader_load_dir(reg, tmp) == AEGIS_OK);
    assert(aegis_skill_registry_count(reg) == 1);
    const aegis_skill_t* s = aegis_skill_registry_get(reg, 0);
    assert(s != NULL);
    assert(strcmp(s->name, "review") == 0);
    assert(strcmp(s->description, "Reviews code.") == 0);
    assert(strstr(s->instructions, "Check style") != NULL);
    assert(strstr(s->path, "/review") != NULL);
    aegis_skill_registry_destroy(reg);
    printf("load_dir PASS\n");

    /* Missing dir is not an error. */
    assert(aegis_skill_registry_create(&reg) == AEGIS_OK);
    assert(aegis_skill_loader_load_dir(reg, "/tmp/skilltest-does-not-exist") == AEGIS_OK);
    assert(aegis_skill_registry_count(reg) == 0);
    aegis_skill_registry_destroy(reg);
    printf("missing_dir PASS\n");

    /* Direct manifest parse: NOT_FOUND without SKILL.md, INVALID on NULLs. */
    {
        char emptydir[1024];
        snprintf(emptydir, sizeof(emptydir), "%s/empty", tmp);
        aegis_skill_t* m = NULL;
        assert(aegis_skill_manifest_parse(emptydir, &m) == AEGIS_ERR_NOT_FOUND);
        assert(m == NULL);
        assert(aegis_skill_manifest_parse(NULL, &m) == AEGIS_ERR_INVALID);
        assert(aegis_skill_manifest_parse(emptydir, NULL) == AEGIS_ERR_INVALID);
    }
    printf("manifest_validate PASS\n");

    remove_skill_dir(tmp, "review", 1);
    remove_skill_dir(tmp, "empty", 0);
    {
        char stray[1024];
        snprintf(stray, sizeof(stray), "%s/notes.txt", tmp);
        assert(unlink(stray) == 0);
    }
    assert(rmdir(tmp) == 0);
    printf("ALL_SKILL_LOADER_TESTS PASSED\n");
    return 0;
}
