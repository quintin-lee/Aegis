# Context

## Role

Context produces `aegis_message_list_t`, not `char*`. Old `aegis_context_build()` (string) kept for `planner` compat; new `aegis_context_build_messages()` is primary for `agent loop`.

## Sources

| Source | Role | Priority |
|---|---|---|
| SYSTEM, TOOL_DEFS, MEMORY | SYSTEM | high |
| GOAL, PLAN, TASK, HISTORY | USER | medium |
| OBSERVATION | TOOL | low |

## Budget

- `token_budget` (0 = unlimited), `reserve_output` (future)
- Sections sorted `priority desc`, `cumulative + seg_tokens > budget → skip` (truncated flag)
- `estimate_tokens = (len+3)/4`, or `item->token_estimate` if provided
- Deterministic, no randomness.

## Compaction

When `cumulative > budget` or `context_overflow` error:

```
history → find boundary → summarize (LLM) → summary message → retain recent N → retry
```

`summary` is `AEGIS_MESSAGE_SUMMARY` role, `parent_id` links to boundary.

## Project

Search order `~/.aegis/AGENTS.md → <project>/AGENTS.md → <cwd>/AGENTS.md` (higher specificity wins), loaded as `SYSTEM` section.

## Thread Safety

Builder is **not** thread-safe; built `message_list` is immutable after `build`.
