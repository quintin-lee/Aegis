# ADR 0002: Error Detail Rollout via Thread-Local Bridge

- Status: accepted (design; producers land per-phase)
- Date: 2026-09-11
- Deciders: aegis maintainers
- Scope: status codes, `aegis_error_t`, TLS bridge, producer wiring

## Context

Three error mechanisms coexist without connection: `aegis_status_t`
(16 codes, returned everywhere, no detail), `aegis_error_t` (heap
object with message + cause chain, implemented and tested, zero
consumers until recently), and as of this session a thread-local
bridge (`aegis_error_set_last` / `aegis_error_last`) with one producer
(tool-argument validation). The `aegis_err_t` / `aegis_status_t`
enumerator collision that blocked the bridge is fixed (`AEGIS_ERROR_*`
namespace); `nm` confirms no stray globals.

## Decision

Bridge pattern, NOT signature changes. Rationale: changing dozens of
`aegis_status_t` return types to detail-carrying results breaks every
caller and test for no behavioral gain; the TLS slot gives the same
observability additively:

- Producers (system boundaries only: provider HTTP, tool exec,
  session IO, model dispatch) publish detail on failure paths and
  return the status code unchanged.
- Lifetime: per-thread slot, valid until the next set on that thread;
  `set(NULL)` clears. Consumers read immediately (errno-style).
- Codes stay split by design: `aegis_status_t` for control flow,
  `aegis_error_t` (`AEGIS_ERROR_*`) for human detail; no 1:1 mapping
  is required or maintained.

## Consequences

- Phases (each: wire + test + green gate, one commit):
  1. Provider sites — attach HTTP status + truncated response body
     excerpt to 429/413/5xx failures (bodies already in hand at all
     three mapping sites).
  2. Tool exec — record tool name + exit/timeout cause (validation
     case already done).
  3. Session save/load — record path + parse position on JSONL
     corruption (today: bare status only).
  4. Model dispatch — record provider name + gate that rejected
     (NOT_FOUND/INVALID/PERM paths).
- Non-goals: changing any return type; global error stacks;
  backtraces; unifying the two enums (kept separate deliberately).
- Risk: stale-slot reads if consumers hold detail across calls —
  documented as misuse in `error.h`; mitigated by the set-on-failure
  convention, not by machinery.
