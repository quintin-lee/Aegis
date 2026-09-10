# ADR 0001: Session / Checkpoint Dual-Track Persistence

- Status: accepted
- Date: 2026-09-10
- Deciders: aegis maintainers
- Scope: `aegis run`, `status`, `inspect`, session JSONL, AEGISCHK checkpoint

## Context

The reactive loop path (coding agent) and the autonomous path
(plan → task graph → scheduler → executor) need different recovery
granularity. A single persistence format served neither well: the
conversation record wants append-only replay, while long autonomous jobs
want atomic crash-safe snapshots. After `aegis run` moved to the coding
agent, `status`/`inspect` showed "no checkpoint" because nothing wrote
AEGISCHK anymore.

## Decision

Two tracks, associated by directory sibling, either may be absent:

- **Session** (`session.jsonl` next to the checkpoint path) is the
  conversation source of truth: append-only JSONL, written best-effort
  after every `run`, read by `status`/`inspect` for history display.
- **Checkpoint** (`checkpoint.bin`, AEGISCHK magic + CRC, atomic
  tmp→rename write) remains the autonomous workflow recovery format,
  written per-task by the autonomous loop only.

Path rule: `cli_session_path_for_checkpoint()` maps any checkpoint path
to its sibling `session.jsonl` (see `apps/aegis/cli_helpers.c`).

## Consequences

- `run` (reactive) writes session only; `status`/`inspect` report each
  track independently and tolerate either missing.
- Session compaction (`coding agent` auto-compact past the 128-message
  loop window) is permanent: persisted history matches what the model
  saw. Fork/replay fidelity beyond the window is intentionally traded
  for bounded memory.
- A corrupt session file fails `status`/`inspect` loudly (exit 1),
  mirroring corrupt-checkpoint handling; a failed session *write* only
  warns so a successful run is never retroactively failed.
- Open: true summarization on compaction (today oldest messages are
  dropped, not summarized); unifying `status` output when both tracks
  exist for one goal.
