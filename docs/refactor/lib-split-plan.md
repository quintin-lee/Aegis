# Library Split Plan: from `aegis_core` Umbrella to Fine-Grained Linking

> Status: plan (not started) · Date: 2026-09-11 · Based on measured `target_link_libraries` graph, not estimates.

## 1. Current state (measured 2026-09-11)

- 25 real static libs + 2 INTERFACE umbrellas (`aegis_core`, `aegis_workflow`).
  `aegis_core` = everything; all ~66 tests and both executables link it.
- Per-lib PUBLIC deps are already fine-grained and essentially acyclic:
  leaves (`common`) → mid (`message`, `task`, `tool`) → high fan-out
  (`agent`: session/model/context/tool/event/runtime/common;
  `autonomous`: planner/scheduler/executor/task/checkpoint/security/critic).
- Known defects found during measurement:
  1. `aegis_autonomous` lists `aegis_planner` TWICE (harmless duplicate).
  2. `aegis_skill` is built and installed but MISSING from the
     `aegis_core` umbrella (today reachable only transitively via
     `aegis_coding PUBLIC`). Same check needed for reflection/replanner
     sources (no dedicated libs found — confirm which target owns them).
  3. `aegis_tool` → PRIVATE `aegis_executor` while `aegis_executor` →
     PUBLIC `aegis_task`: legal but the one non-obvious edge; document,
     do not "fix".
  4. `aegis_coding` → PRIVATE `aegis_llm_openai`: verify it sits inside
     `if(AEGIS_OPENAI_PROVIDER)` (builds today, so presumably yes).
  5. Static archives make link ORDER significant (see commit
     "fix checkpoint link": checkpoint→planner edge was latent until the
     CLI demand chain changed). Any phase must re-verify with a clean
     configure+build, not just incremental.

## 2. Decision

Keep `aegis_core` FOREVER as a compatibility alias (external consumers
must not break). The split means: new/converted targets link
fine-grained libs; the umbrella stays for legacy. No big-bang removal.

## 3. Phases (each ends: clean configure + build + ctest + nm rescan)

- Phase 0 — Audit (no behavior change): confirm reflection/replanner
  target ownership; confirm skill membership intent; draw the DAG and
  check acyclicity; record `aegis_shell` and each test's true link
  closure (starting point: `fuzz_session_standalone` needs only
  session+message+common; `unit_error` only common).
- Phase 1 — Correctness (additive): fix the duplicate planner entry;
  add missing libs to umbrellas where the audit says so; dedupe
  `AegisInstall.cmake` against the real lib list (extension already
  removed in an earlier change — re-verify).
- Phase 2 — Pilot (2 targets): convert `fuzz_session_standalone` and
  `unit_error` to fine-grained links; extend `aegis_add_test` with an
  optional LIBS parameter defaulting to `aegis_core` so rollout is
  per-call, never flag-day.
- Phase 3 — Rollout per test group (common → session/message →
  provider/tool → agent/coding → system), one commit per group, each
  green before the next.
- Phase 4 — Executables (`aegis`, `aegis_shell`, coding-loop drivers)
  last; keep `aegis_core` alias in place permanently.

## 4. Non-goals / risks

- No merging or renaming of libs in this plan (separate decision).
- No shared-library conversion: static archives keep link-order
  sensitivity, so every phase needs the clean-configure gate.
- `aegis_tool` ↔ `aegis_executor` edge stays as-is (forward-declared
  headers already break the cycle at source level).
