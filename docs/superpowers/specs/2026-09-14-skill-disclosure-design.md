# Skill Progressive-Disclosure Design

**Feature:** pi-agent gap #4 — close the last remaining pi-agent gap by making the
coding agent's loaded skills actually usable: expose skill metadata to the model and
load full skill instructions on demand, instead of pre-loading every instruction body
into context.

**Status:** Approved brainstorming (sections §1–§4). No new CMake option; `aegis_coding`
already links `aegis_skill`.

---

## Background

The skill system (`src/skill/`, `include/aegis/skill/`) is a **loading framework**:
`aegis_skill_loader_load_dir` scans a directory for subdirectories containing
`SKILL.md`/`skill.md` and parses each into an `aegis_skill_t` (name, description,
instructions, path) stored in an `aegis_skill_registry_t`. The coding agent loads
these into `a->skills` at create time (best-effort, from `$HOME/.aegis/skills` and
`<project_root>/.aegis/skills`).

**The gap:** the skills are loaded but **never consumed**. No skill metadata appears in
the model's system prompt, no tool exposes a skill's full instructions, and there is no
on-demand lookup. pi-style *progressive disclosure* means: the model always sees the
cheap skill **metadata** (name + one-line description), and only when it decides a skill
is relevant does it request the **full instructions** for that one skill — instead of
injecting every instruction body up front.

This design adds exactly that: metadata injection into the system prompt plus a
`use_skill` tool that fetches one skill's instructions by name on demand.

## Goals

1. Inject the list of available skills (name + description) into the coding agent's
   system prompt so the model knows what skills exist.
2. Provide a `use_skill` tool that returns the full instructions for a named skill.
3. Keep backward compatibility: an agent with no skills gets the plain base prompt.
4. No new dependencies, no new CMake option.

## Non-Goals (out of scope)

- Multi-skill batch loading, skill capability gating, or context-aware skill selection.
- Persisting "used skill" state across turns.
- Changes to the skill file format (`SKILL.md` parsing stays as-is).
- Thread-safety of the skill registry (it remains not thread-safe, matching current state).

---

## §1 — System-prompt metadata injection

Add a helper that builds the coding agent's system prompt from the loaded skills.

```c
/**
 * @brief Build the coding agent system prompt, embedding skill metadata.
 *
 * Produces a heap-allocated string: the base coding-agent prompt plus, when @p
 * skills is non-empty, a block listing each available skill as
 * "- <name>: <description>" and a pointer to the "use_skill" tool. The caller
 * owns and frees the returned string.
 *
 * @param[in]  skills Loaded skill registry (may be NULL → base prompt only).
 * @param[out] out    Receives the malloc'd prompt string on success.
 * @return AEGIS_OK, or AEGIS_ERR_NOMEM / AEGIS_ERR_INVALID.
 */
aegis_status_t build_coding_system_prompt(aegis_skill_registry_t* skills, char** out);
```

Behavior:

- **No skills** (NULL registry or count 0): returns a copy of the base
  `CODING_AGENT_SYSTEM_PROMPT` (`"You are a coding agent. Use tools to help the user."`).
  This keeps `system_coding_loop` and the no-skill path green.
- **Skills present:** base prompt + appended block:

  ```
  Available skills (load full instructions with the "use_skill" tool):
  - <name>: <description>
  - <name>: <description>
  ...
  ```

  One line per skill (name from `aegis_skill_t.name`, description from
  `aegis_skill_t.description`, which may be empty — an empty description renders as
  `- <name>:` with nothing after the colon).

Location: a new small module `src/coding/system_prompt.c` (or folded into the new
`skill_tools.c`). Exposes `build_coding_system_prompt` plus the base-prompt constant.

The coding agent stores the result in a new struct field `char* system_prompt_owned`
(heap-owned, freed in `destroy`) and uses it in place of the three static
`CODING_AGENT_SYSTEM_PROMPT` references (create loop cfg, `replace_session`,
`set_model_client` rebuilds).

## §2a — Skill registry by-name lookup

Add a find helper to the skill registry (it currently only has index-based `get`).

```c
/**
 * @brief Find a skill in the registry by name.
 *
 * Linear scan over the registry. On success @p out receives a borrowed pointer to
 * the stored skill (valid until the registry is modified/destroyed, matching
 * aegis_skill_registry_get). Not thread-safe.
 *
 * @return AEGIS_OK on match, AEGIS_ERR_NOT_FOUND if no skill named @p name,
 *   AEGIS_ERR_INVALID if @p reg or @p name is NULL.
 */
aegis_status_t aegis_skill_registry_find(const aegis_skill_registry_t* reg,
                                         const char* name,
                                         const aegis_skill_t** out);
```

Implementation: `aegis_skill_registry_count` + `aegis_skill_registry_get(i)` +
`strcmp(skill->name, name)`; `*out` set to the first match.

## §2b — `use_skill` tool module

New module `src/coding/skill_tools.c` + `include/aegis/coding/skill_tools.h`,
mirroring the `delegate_tools` pattern (a heap tool def carrying a context blob,
since a const global cannot carry the registry pointer).

```c
/** Context the use_skill execute fn reads; the skills registry is borrowed. */
typedef struct {
    aegis_skill_registry_t* skills;
} skill_tools_ctx_t;

/** Register a heap-allocated "use_skill" tool into @p reg; caller owns the def+ctx. */
aegis_status_t aegis_coding_skill_tools_register(aegis_tool_registry_t* reg,
                                                 skill_tools_ctx_t*     ctx,
                                                 aegis_tool_def_t**     out_def);

/** Free a def + ctx from aegis_coding_skill_tools_register (NULL-safe). */
void aegis_coding_skill_tools_free(aegis_tool_def_t* def, skill_tools_ctx_t* ctx);
```

- Tool name: `use_skill`. Single param `name` (STRING, required).
- Execute: read the `name` arg → `aegis_skill_registry_find(ctx->skills, name, &sk)`.
  - **Found:** `aegis_tool_result_set_string(out, sk->instructions ? sk->instructions : "")`
    → return `AEGIS_OK`.
  - **Not found:** return `AEGIS_ERR_NOT_FOUND` (result left zero-valued).
- The `aegis_tool_def_t` is heap-owned (strdup'd name/description + owned param spec
  array), `.user = ctx`, registered via `aegis_tool_registry_register`. `free` frees the
  owned strings/params + def + ctx (NULL-safe; `(void*)` casts for the `const char*`
  ABI fields, as in `delegate_tools`).

## §3 — Coding-agent wiring

Mirror the `delegate_tools` integration (already in `coding_agent.c`):

Struct fields (next to `task_tool`/`subagent`):

```c
char*             system_prompt_owned;
aegis_tool_def_t* skill_tool;
skill_tools_ctx_t* skill_ctx;
```

`create()`, after the skill load and the delegate block:

1. `build_coding_system_prompt(a->skills, &a->system_prompt_owned)` — NOMEM → shared
   `fail:` label.
2. `a->skill_ctx = calloc(1, sizeof(*a->skill_ctx)); a->skill_ctx->skills = a->skills;`
3. `aegis_coding_skill_tools_register(a->tools, a->skill_ctx, &a->skill_tool)` — fail →
   `free(a->skill_ctx)` + `goto fail:`.
4. Replace the three `CODING_AGENT_SYSTEM_PROMPT` references with
   `a->system_prompt_owned`.

`destroy()` (after `aegis_coding_delegate_tools_free`, before session destroy):

```c
aegis_coding_skill_tools_free(a->skill_tool, a->skill_ctx);
free(a->system_prompt_owned);
a->skill_tool = a->skill_ctx = NULL;
a->system_prompt_owned = NULL;
```

## §4 — Testing

Plain-C test `tests/unit/test_skill_tools.c` (assert + `printf("PASS")` + `main`,
repo style):

- Create a temp skill dir with `SKILL.md` (first line = description, rest =
  instructions), load it into a registry with `aegis_skill_loader_load_dir`.
- **Case 1 (find):** `aegis_skill_registry_find(reg, "<name>", &sk)` → `AEGIS_OK`,
  `sk->name` matches, `sk->instructions` matches the body; an unknown name →
  `AEGIS_ERR_NOT_FOUND`.
- **Case 2 (prompt):** `build_coding_system_prompt` with the loaded skill embeds
  `- <name>: <description>` and a `use_skill` mention; with an empty/NULL registry it
  equals the base prompt.
- **Case 3 (use_skill tool):** `aegis_coding_skill_tools_register` into a tool
  registry, then `aegis_tool_execute(reg, "use_skill", args{name="demo"}, token,
  &result)` → `result` string == the skill's instructions; an unknown `name` →
  `AEGIS_ERR_NOT_FOUND`.

`cmake/AegisTests.cmake`: add `unit_skill_tools` target linking
`aegis_coding aegis_skill aegis_tool aegis_common aegis_agent aegis_session aegis_model pthread`.

Regression: `tests/integration/test_cli.c` asserts a "registered tools (N):" count —
the new `use_skill` tool bumps it by one; update that expectation. `unit_loop` /
`system_coding_loop` must stay green (no-skill path returns the base prompt).

## Files touched

| File | Change |
|---|---|
| `include/aegis/skill/registry.h` | Declare `aegis_skill_registry_find` |
| `src/skill/registry.c` | Implement `aegis_skill_registry_find` |
| `include/aegis/coding/skill_tools.h` | New: `skill_tools_ctx_t`, register/free, `build_coding_system_prompt` |
| `src/coding/skill_tools.c` | New: `use_skill` tool + `build_coding_system_prompt` |
| `src/coding/CMakeLists.txt` | Add `skill_tools.c` to `aegis_coding` sources |
| `src/coding/coding_agent.c` | Struct fields + create/destroy wiring + replace 3 prompt refs |
| `tests/unit/test_skill_tools.c` | New plain-C test |
| `tests/integration/test_cli.c` | Bump registered-tools count expectation |
| `cmake/AegisTests.cmake` | Register `unit_skill_tools` target |

## Verification

- Full `ctest` green (expect +1 new test; no regressions).
- `clang-format --dry-run` clean on the new/changed C files (never on `.cmake`).
- No-skill path returns the base prompt (backward compat).
