# Session

## Model

```c
struct aegis_session {
  id[37], branch_id[37], parent_id[37],
  project_root, created_at, updated_at,
  message_list* messages
}
```

- `id`/`branch_id` UUID v4, `parent_id` for fork.
- `messages` is `aegis_message_list_t` (ordered).

## Lifecycle

`create(project_root) → append_message* → save(path) → load(path) → fork() → destroy`

## Persistence (JSONL)

Append-only, crash-tolerant, replayable:

```json
{"type":"session_start","v":1,"id":"...","branch":"...","parent":"","created":...,"project":"..."}
{"type":"message","id":"...","role":"user","content":"hello"}
{"type":"tool_call","msg_id":"...","call_id":"...","name":"read","args":"{\"path\":\"a.txt\"}"}
```

`save` writes to `*.tmp` then `rename` (atomic). `load` replays lines, tolerates missing `tool_call` lines (forward compat).

## Branch / Fork

```
root
 ├─ turn1
 ├─ turn2
 └─ branchA (fork at turn2) → new id, parent=root.id, messages cloned
```

`fork` copies `project_root` and clones `message_list`, new `branch_id`.

## Compaction (future)

`history → boundary → summarize → summary message + recent tail` (automatic on `CONTEXT_OVERFLOW`).

## Ownership

`session_create` owns `project_root` copy and `message_list`; `append` clones `message`; `save` borrows `path`; `load` transfers ownership of new `session`.
