# Loop-Strategy Convergence Design

> Date: 2026-09-09 · Status: design (not implemented) · Follows P1-8 direction decision

## 1. Current state (verified in tree)

- `include/aegis/agent/strategy.h` declares a loop-strategy ABI
  (`init/before_turn/after_model/after_tool/should_continue/shutdown`),
  but: `void* loop` is untyped, no `ABI_VERSION`, no ownership/thread-safety
  contract, no registry, and **nothing includes it except**
  `src/strategy/autonomous_strategy.c`.
- `src/strategy/autonomous_strategy.c` (66 lines) wraps the autonomous agent
  and exposes a def, but **all four turn hooks are NULL** — only
  `init/shutdown` are populated. It is a handle wrapper, not a strategy.
- `src/agent/loop.c` never invokes any strategy hook; `loop_config` has no
  strategy field. The reactive loop (`run_turn`: context → model stream →
  tool exec → session append) is hard-wired to the coding-agent flow.
- `src/strategy/strategy_registry.c` (193 lines) serves the **plan-level**
  `aegis_strategy_def_t` (strategy.h, ABI v1) — a different concept with a
  confusingly similar name. The two registries/ABIs must not be merged;
  plan-level stays as the planner's policy plug-point.

Net: the loop-strategy ABI is declared but dead. Autonomous remains a
separate `aegis_autonomous_agent_run(goal)` entry point used by `cli_run.c`.

## 2. Target

One reactive `aegis_agent_loop_t` with an optional loop-strategy:

- No strategy (NULL def) → today's coding-agent behavior, byte-identical.
- `AutonomousStrategy` set → goal-driven plan→execute→evaluate→reflect→replan
  runs *inside* the loop's turn structure instead of a parallel orchestrator.
- `autonomous_agent.h` stays as a thin adapter (deprecated in a later step,
  not this one) so `cli_run.c` and system tests keep working throughout.

## 3. ABI deltas (`agent/strategy.h`, additive where possible)

1. Forward-declare `struct aegis_agent_loop` and replace `void* loop` with
   `aegis_agent_loop_t*` (borrowed, locks never held during hooks — same
   rule as `on_event`/`tool_approval` in loop.h).
2. Add `AEGIS_AGENT_STRATEGY_ABI_VERSION 1u` + `abi_version` field; loop
   rejects mismatched defs with `AEGIS_ERR_INVALID`.
3. Document ownership: def strings borrowed (must outlive use), `user`
   owned by whoever created the strategy object; all hooks optional
   (NULL = no-op) except `name`.
4. Add `on_state` hook? **No** — observer events already cover state
   visibility. Keep the ABI to six hooks.

Hook semantics (all invoked without loop locks held):

- `init(user)` — once at `loop_create` with strategy (fail → create fails).
- `before_turn(user, loop)` — before context build; may return
  `AEGIS_ERR_CANCELLED` to abort the turn.
- `after_model(user, loop)` — after assistant message appended, before tool
  dispatch; strategy may inspect/annotate via loop accessors.
- `after_tool(user, loop)` — after each tool result appended.
- `should_continue(user, *out)` — consulted after a turn ends with no
  pending tool calls: `1` = run another turn (autonomous: plan incomplete),
  `0` = return to caller. Loop caps consecutive strategy-continued turns
  with a `max_strategy_turns` config (default 10) to bound runaway loops.
- `shutdown(user)` — once at `loop_destroy`.

## 4. Loop wiring (`src/agent/loop.c`, additive)

- `loop_config` gains `const aegis_agent_strategy_def_t* strategy`
  (borrowed, NULL = current behavior).
- `run_turn` invokes hooks at the four points; `run` consults
  `should_continue` when the model stops emitting tool calls.
- Hook failure (non-OK, non-CANCELLED) aborts the turn with that status;
  CANCELLED propagates as today.
- Observer events unchanged; strategy hooks fire before/after the
  corresponding event emission so observers see a consistent order.

## 5. AutonomousStrategy implementation

Fill the NULL hooks in `autonomous_strategy.c` by delegating to the
existing autonomous phases (planning.c / execution.c / evaluation.c /
reflection.c / replanning.c via the internal runtime):

- `before_turn` — ensure a fresh plan/graph exists for the goal
  (replaces the outer `for iteration` setup in `autonomous_loop_run`).
- `after_tool` — feed tool observations into the task graph state
  (same `scheduler notify_complete` path as `execution.c`).
- `should_continue` — run critic evaluation; `1` while verdict != SUCCESS
  and iterations remain (replan via `replanning.c` when needed).
- `after_model` — NULL for autonomous (reactive-only concern).
- Checkpointing stays per-task as today (`checkpoint.c`).

`cli_run.c` keeps calling `autonomous_agent_run` (adapter); a later step
switches it to loop+strategy and marks the old API deprecated.

## 6. Naming disambiguation (required in the same change)

- Rename files, not concepts: `src/strategy/` (plan-level registry) stays;
  new loop-strategy code lives in `src/agent/strategy.c` next to loop.c.
- Doc comment at the top of both `strategy.h` files cross-referencing the
  other ("plan-level policy" vs "loop lifecycle plug-point").

## 7. Tests (TDD, per hook)

- `tests/unit/test_loop_strategy.c`: NULL-def parity (byte-identical event
  sequence vs no-strategy run), hook call order, hook-abort propagation,
  `should_continue` cap, ABI mismatch rejection.
- `tests/system/test_autonomous_strategy_e2e.c`: goal → plan → mock-tool
  execute → critic SUCCESS via loop+AutonomousStrategy (reuse fixtures from
  `test_autonomous_closed_loop.c`).
- Existing `system_autonomous*` + `unit_strategy` suites must stay green
  (adapter untouched).

## 8. Phases (each ends build + ctest 61+ green + commit)

1. ABI hardening only (typed loop, version, docs) — no behavior change.
2. Loop wiring + NULL-def parity tests.
3. AutonomousStrategy hooks + e2e test.
4. Naming cross-refs + `cli_run` adapter note (no CLI switch yet).

## 9. Risks

- `void* loop` → typed forward decl creates `agent/loop.h` ↔
  `agent/strategy.h` include cycle → solve with forward declaration only
  (strategy.h must not include loop.h; loop.c includes both).
- `should_continue` runaway → bounded by `max_strategy_turns`, plus the
  existing cancellation token path stays authoritative.
- Two "strategy" concepts confusing reviewers → the cross-ref comments in
  §6 are a merge requirement, not nice-to-have.
