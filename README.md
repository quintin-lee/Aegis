# Aegis — Modular Agent Runtime

**Aegis** is a C11 agent runtime that provides a modular, memory-safe foundation for building autonomous planning agents.  
Core guarantees: correct ownership, strict module boundaries, stable ABI, explicit error propagation, and full lifecycle coverage.

## Architecture

```
Foundation
   ↓
Core Runtime
   ↓
Planner / Executor / Memory
   ↓
Provider / Tool / Storage
   ↓
Plugin / Application
```

Layered isolation: core never depends on any concrete provider (OpenAI, Qwen, SQLite, etc.).  
Everything enters through stable interfaces declared in `include/aegis/`.

## Build

```bash
cmake -S . -B build
cmake --build build
```

### Options

| Variable               | Default | Description                          |
|------------------------|---------|--------------------------------------|
| `AEGIS_BUILD_TESTS`    | ON      | Build test suite (requires GTest)    |
| `AEGIS_BUILD_BENCH`    | OFF     | Build benchmark targets              |
| `AEGIS_BUILD_DOCS`     | OFF     | Generate Doxygen documentation       |
| `AEGIS_WARNINGS_AS_ERRORS` | ON  | Warnings promoted to errors          |

### Tests

```bash
cmake -S . -B build -DAEGIS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizer builds (append after `cmake ..`):

```bash
# Address sanitizer
cmake -S . -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address -g"
# Thread sanitizer
cmake -S . -B build-tsan -DCMAKE_C_FLAGS="-fsanitize=thread -g"
```

## Public API

All public symbols live under `include/aegis/` with the `aegis_` prefix.

```c
// opaque types — never expose struct layouts
typedef struct aegis_agent  aegis_agent_t;
typedef struct aegis_task   aegis_task_t;
typedef struct aegis_event  aegis_event_t;
```

Ownership semantics are explicit on every pointer parameter/return value:
`owned`, `borrowed`, `retained`, `transferred`, `shared`, `weak`.

## Guidelines

See [`AGENTS.md`](./AGENTS.md) for the complete AI-coding约束 enforced on all contributors (human or agent).

## Directory Layout

```
include/aegis/   ← Public ABI headers
src/             ← Implementation
src/internal/    ← Internal headers (not exposed)
tests/unit/      ← Fast isolated tests
tests/integration/ ← End-to-end flow tests
benchmarks/      ← Performance benchmarks
docs/            ← Design docs, ADRs
cmake/           ← CMake modules and package config template
.github/workflows/ ← CI pipelines
```

## License

Proprietary — see LICENSE file.
