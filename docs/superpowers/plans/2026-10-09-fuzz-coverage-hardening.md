# Fuzz & Coverage Hardening Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the two verified test-infrastructure gaps: add fuzz harnesses for the two untrusted-input parsers (MCP JSON DOM, checkpoint deserializer) and add a CI coverage job with a line-coverage threshold gate.

**Architecture:** Two new standalone fuzz harnesses mirror the existing `tests/fuzz/fuzz_session.c` pattern (libFuzzer `LLVMFuzzerTestOneInput` + `FUZZ_STANDALONE` fixed-sample `main` registered as a regular ctest). A new `coverage` CI job configures with the existing `cmake/AegisCoverage.cmake` (`AEGIS_ENABLE_COVERAGE=ON`), runs the full suite, extracts `src/` line coverage with lcov, gates on a threshold, and uploads an HTML report artifact.

**Tech Stack:** C11, CMake (existing `add_test`/`aegis_set_warnings` helpers), libFuzzer-style harnesses in standalone mode, lcov/genhtml, GitHub Actions.

**Context:** P1 originally listed 4 items; items 4 (full-chain e2e) and 5 (reflection/replanner tests) were verified as ALREADY COVERED (`tests/system/test_coding_loop.c`, `test_openai_tool_loop_e2e.c`, `test_anthropic_tool_loop_e2e.c`, `tests/unit/test_planner.c:250-340`, `tests/unit/test_planner_llm.c:193-221`) and are intentionally NOT in this plan.

---

## Chunk 1: Fuzz harnesses

### Task 1: fuzz harness for MCP JSON parser

**Files:**
- Create: `tests/fuzz/fuzz_mcp_json.c`
- Modify: `cmake/AegisTests.cmake` (fuzz block, currently lines ~392-400)

- [ ] **Step 1: Create the harness**

Create `tests/fuzz/fuzz_mcp_json.c`:

```c
/**
 * @file fuzz_mcp_json.c
 * @brief libFuzzer harness over the MCP JSON DOM parser: parse →
 *        serialize → re-parse round-trip on byte-mutated inputs;
 *        FUZZ_STANDALONE builds a fixed-sample main so the corpus runs
 *        as a regular ctest without a fuzzer engine.
 */
#include "mcp_json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > (1u << 20)) {
        return 0;
    }
    char* json = malloc(size + 1);
    if (!json) {
        return 0;
    }
    memcpy(json, data, size);
    json[size] = '\0';

    aegis_json_value_t* v = NULL;
    if (aegis_json_parse(json, &v) == AEGIS_OK && v) {
        char* out = NULL;
        if (aegis_json_serialize(v, &out) == AEGIS_OK && out) {
            /* Round-trip invariant: serialized output must re-parse. */
            aegis_json_value_t* v2 = NULL;
            if (aegis_json_parse(out, &v2) == AEGIS_OK && v2) {
                aegis_json_value_destroy(v2);
            }
            free(out); /* Heap-owned per mcp_json.h contract. */
        }
        aegis_json_value_destroy(v);
    }
    free(json);
    return 0;
}

#ifdef FUZZ_STANDALONE
int main(void)
{
    const char* samples[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"ok\":true}}",
        "[1,2,3,{\"a\":null},[[]]]",
        "\"\\u00e9\\u0041\\n\\t\\\\\\/\"",
        "-0.5e-10",
        "not json \xFF\xFE",
        "",
        "{\"truncated\":",
        "n",
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        LLVMFuzzerTestOneInput((const uint8_t*)samples[i], strlen(samples[i]));
    }
    printf("fuzz standalone PASS\n");
    return 0;
}
#endif
```

Note: keep nesting in standalone samples shallow (<100 levels). Deep-nesting stress belongs to a real libFuzzer run, not to ctest — a recursive-descent parser may legitimately exhaust the stack at 10k+ depth and that must not fail CI unexpectedly.

- [ ] **Step 2: Register in CMake**

In `cmake/AegisTests.cmake`, insert immediately after the `fuzz_session_standalone` block (after its `add_test` line, keeping the existing `AEGIS_ENABLE_ASAN` property group extended):

```cmake
    add_executable(fuzz_mcp_json_standalone tests/fuzz/fuzz_mcp_json.c)
    target_compile_definitions(fuzz_mcp_json_standalone PRIVATE FUZZ_STANDALONE)
    target_link_libraries(fuzz_mcp_json_standalone PRIVATE aegis_json aegis_common)
    target_include_directories(fuzz_mcp_json_standalone PRIVATE ${PROJECT_SOURCE_DIR}/src/mcp)
    aegis_set_warnings(fuzz_mcp_json_standalone)
    add_test(NAME fuzz_mcp_json_standalone COMMAND fuzz_mcp_json_standalone)
```

Also add to the existing `if(AEGIS_ENABLE_ASAN)` property group:

```cmake
    set_tests_properties(fuzz_mcp_json_standalone PROPERTIES ENVIRONMENT "LD_PRELOAD=;ASAN_OPTIONS=verify_asan_link_order=0")
```

- [ ] **Step 3: Build**

Run: `cmake -S . -B build && cmake --build build -j$(nproc) --target fuzz_mcp_json_standalone`
Expected: builds without warnings/errors.

- [ ] **Step 4: Run the test**

Run: `ctest --test-dir build -R fuzz_mcp_json --output-on-failure`
Expected: `fuzz_mcp_json_standalone` PASS, output contains `fuzz standalone PASS`.

- [ ] **Step 5: Format (CI formatting job gates on this)**

Run: `clang-format -i tests/fuzz/fuzz_mcp_json.c && clang-format --dry-run --Werror tests/fuzz/fuzz_mcp_json.c`
Expected: no output (passes). The CI `formatting` job runs `clang-format --dry-run --Werror` over all `.c` files — an unformatted new file breaks CI.

- [ ] **Step 6: Commit**

```bash
git add tests/fuzz/fuzz_mcp_json.c cmake/AegisTests.cmake
git commit -m "test(fuzz): add libFuzzer harness for MCP JSON parser"
```

### Task 2: fuzz harness for checkpoint deserializer

**Files:**
- Create: `tests/fuzz/fuzz_checkpoint.c`
- Modify: `cmake/AegisTests.cmake` (same fuzz block)

- [ ] **Step 1: Create the harness**

Create `tests/fuzz/fuzz_checkpoint.c`:

```c
/**
 * @file fuzz_checkpoint.c
 * @brief libFuzzer harness over the checkpoint text deserializer:
 *        deserialize → serialize → re-deserialize round-trip on
 *        byte-mutated inputs; FUZZ_STANDALONE builds a fixed-sample
 *        main so the corpus runs as a regular ctest without a fuzzer
 *        engine.
 */
#include "aegis/checkpoint/checkpoint.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > (1u << 20)) {
        return 0;
    }
    char* text = malloc(size + 1);
    if (!text) {
        return 0;
    }
    memcpy(text, data, size);
    text[size] = '\0';

    aegis_checkpoint_t* ck = NULL;
    if (aegis_checkpoint_deserialize(text, &ck) == AEGIS_OK && ck) {
        char* out = NULL;
        if (aegis_checkpoint_serialize(ck, &out) == AEGIS_OK && out) {
            /* Round-trip invariant: serialized output must re-parse. */
            aegis_checkpoint_t* ck2 = NULL;
            if (aegis_checkpoint_deserialize(out, &ck2) == AEGIS_OK && ck2) {
                aegis_checkpoint_destroy(ck2);
            }
            free(out); /* Heap-owned per checkpoint.h contract. */
        }
        aegis_checkpoint_destroy(ck);
    }
    free(text);
    return 0;
}

#ifdef FUZZ_STANDALONE
int main(void)
{
    /* Build one valid checkpoint in-memory as the seed sample. */
    aegis_checkpoint_t* seed_ck = NULL;
    char*               seed    = NULL;
    if (aegis_checkpoint_create(&seed_ck) == AEGIS_OK &&
        aegis_checkpoint_set_goal(seed_ck, "fuzz seed") == AEGIS_OK &&
        aegis_checkpoint_serialize(seed_ck, &seed) == AEGIS_OK && seed) {
        LLVMFuzzerTestOneInput((const uint8_t*)seed, strlen(seed));
        free(seed);
    }
    aegis_checkpoint_destroy(seed_ck);

    const char* samples[] = {
        "",
        "garbage \xFF\xFE",
        "AEGISCHK v1\nGOAL=fuzz\n", /* real magic (checkpoint.h) + truncated body */
        "{\"goal\":\"x\"",
        "AEGISCHK v99999999999999999999\n",
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        LLVMFuzzerTestOneInput((const uint8_t*)samples[i], strlen(samples[i]));
    }
    printf("fuzz standalone PASS\n");
    return 0;
}
#endif
```

Note: seed sample content is best-effort — if `serialize` on a freshly created checkpoint fails, the harness still runs the static samples. The exact sample strings may be adjusted to match the real on-disk format after inspecting `src/checkpoint/checkpoint.c` serialization output during execution.

- [ ] **Step 2: Register in CMake**

In `cmake/AegisTests.cmake`, insert after the `fuzz_mcp_json_standalone` block from Task 1:

```cmake
    add_executable(fuzz_checkpoint_standalone tests/fuzz/fuzz_checkpoint.c)
    target_compile_definitions(fuzz_checkpoint_standalone PRIVATE FUZZ_STANDALONE)
    target_link_libraries(fuzz_checkpoint_standalone PRIVATE aegis_checkpoint)
    aegis_set_warnings(fuzz_checkpoint_standalone)
    add_test(NAME fuzz_checkpoint_standalone COMMAND fuzz_checkpoint_standalone)
```

Also add to the `if(AEGIS_ENABLE_ASAN)` property group:

```cmake
    set_tests_properties(fuzz_checkpoint_standalone PROPERTIES ENVIRONMENT "LD_PRELOAD=;ASAN_OPTIONS=verify_asan_link_order=0")
```

(`aegis_checkpoint` PUBLICly links `aegis_task aegis_planner aegis_common`, so no extra link libraries are needed.)

- [ ] **Step 3: Build**

Run: `cmake -S . -B build && cmake --build build -j$(nproc) --target fuzz_checkpoint_standalone`
Expected: builds without warnings/errors.

- [ ] **Step 4: Run the test**

Run: `ctest --test-dir build -R fuzz_checkpoint --output-on-failure`
Expected: PASS, output contains `fuzz standalone PASS`.

- [ ] **Step 5: Format (CI formatting job gates on this)**

Run: `clang-format -i tests/fuzz/fuzz_checkpoint.c && clang-format --dry-run --Werror tests/fuzz/fuzz_checkpoint.c`
Expected: no output (passes).

- [ ] **Step 6: Verify full suite still green**

Run: `ctest --test-dir build -E bench_ --output-on-failure`
Expected: `100% tests passed` (now 79 tests).

- [ ] **Step 7: Commit**

```bash
git add tests/fuzz/fuzz_checkpoint.c cmake/AegisTests.cmake
git commit -m "test(fuzz): add libFuzzer harness for checkpoint deserializer"
```

---

## Chunk 2: Coverage CI job

### Task 3: coverage job with threshold gate

**Files:**
- Modify: `.github/workflows/ci.yml` (add `coverage` job)

- [ ] **Step 1: Measure baseline coverage locally**

Local machine has lcov 2.5 (`/usr/bin/lcov`). Run the exact pipeline the CI job will run:

```bash
cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DAEGIS_BUILD_TESTS=ON -DAEGIS_ENABLE_COVERAGE=ON
cmake --build build-coverage -j$(nproc)
ctest --test-dir build-coverage --output-on-failure -E bench_
lcov --capture --directory build-coverage --output-file coverage.info
lcov --extract coverage.info '*/src/*' --output-file coverage-src.info --ignore-errors unused
lcov --summary coverage-src.info
```

Expected: tests pass; summary prints a line-coverage percentage. Record it as `PCT`.

Notes:
- If lcov reports version-specific errors (e.g. `mismatch`, `unused`, `empty`, `gcov`), add the appropriate `--ignore-errors <code>` flags and keep only the flags that succeeded locally.
- **Carry any extra `--ignore-errors` flags proven here into the CI YAML's `Collect coverage` step before running Step 4.**
- Extract `*/src/*` only (exclude `tests/`, `include/` has no code, system headers excluded by capture defaults).

- [ ] **Step 2: Fill the threshold placeholder**

When writing the job in Step 3, replace `<MEASURED_FLOOR>` with `floor(PCT)` — the measured line coverage rounded down to a whole percent — so the gate is green today and catches regressions. Record the measured value for the commit message (Step 6).

- [ ] **Step 3: Add the coverage job to `.github/workflows/ci.yml`**

Append after the `formatting` job:

```yaml
  coverage:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake gcc lcov

      - name: Configure
        run: |
          cmake -S . -B build-coverage \
            -DCMAKE_BUILD_TYPE=Debug \
            -DAEGIS_BUILD_TESTS=ON \
            -DAEGIS_ENABLE_COVERAGE=ON

      - name: Build
        run: cmake --build build-coverage -j$(nproc)

      - name: Test
        run: ctest --test-dir build-coverage --output-on-failure -E bench_

      - name: Collect coverage
        run: |
          lcov --capture --directory build-coverage --output-file coverage.info
          lcov --extract coverage.info '*/src/*' --output-file coverage-src.info --ignore-errors unused
          lcov --list coverage-src.info
          PCT=$(lcov --summary coverage-src.info 2>&1 \
            | sed -n 's/.*lines.*: \([0-9.]*\)%.*/\1/p')
          # Fail closed if either number is missing/malformed (e.g. unreplaced placeholder).
          if ! [[ "$PCT" =~ ^[0-9]+(\.[0-9]+)?$ && "$COVERAGE_THRESHOLD" =~ ^[0-9]+$ ]]; then
            echo "invalid coverage or threshold: PCT='$PCT' THRESHOLD='$COVERAGE_THRESHOLD'"
            exit 1
          fi
          echo "Line coverage: ${PCT}% (threshold: ${COVERAGE_THRESHOLD}%)"
          awk -v p="${PCT}" -v t="${COVERAGE_THRESHOLD}" 'BEGIN { exit !(p >= t) }'
        env:
          COVERAGE_THRESHOLD: <MEASURED_FLOOR>   # replaced when writing this job (Step 2)

      - name: Generate HTML report
        if: always()
        run: genhtml coverage-src.info --output-directory coverage-html || true

      - name: Upload coverage artifact
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: coverage-report
          path: coverage-html/
          if-no-files-found: ignore
```

- [ ] **Step 4: Re-run the local pipeline with the final job commands**

Run the exact command sequence from the `Collect coverage` step (with `COVERAGE_THRESHOLD` set) in the repo root. Expected: exit code 0, prints `Line coverage: X%`.

Also verify the gate actually fails when below threshold:

```bash
COVERAGE_THRESHOLD=100 bash -c 'PCT=$(lcov --summary coverage-src.info 2>&1 | sed -n "s/.*lines.*: \([0-9.]*\)%.*/\1/p"); awk -v p="$PCT" -v t="$COVERAGE_THRESHOLD" "BEGIN { exit !(p >= t) }"'
echo $?   # Expected: non-zero
```

And verify the fail-closed guard rejects a malformed threshold (expect exit 1 and the `invalid coverage or threshold` message):

```bash
COVERAGE_THRESHOLD='<MEASURED_FLOOR>' bash -c 'PCT=$(lcov --summary coverage-src.info 2>&1 | sed -n "s/.*lines.*: \([0-9.]*\)%.*/\1/p"); if ! [[ "$PCT" =~ ^[0-9]+(\.[0-9]+)?$ && "$COVERAGE_THRESHOLD" =~ ^[0-9]+$ ]]; then echo "invalid coverage or threshold"; exit 1; fi'
echo $?   # Expected: 1
```

- [ ] **Step 5: Update `.gitignore` and clean up**

Facts (verified): `coverage.info` is already ignored (`.gitignore:44`, after the Valgrind section); `coverage-src.info` is **not**; `build-coverage/` is **not** (only `build/`, `build-asan/`, `build-tsan/` are).

Append to `.gitignore`:

```
coverage-src.info
build-coverage/
```

Then clean up local artifacts:

```bash
rm -rf build-coverage coverage.info coverage-src.info coverage-html
```

- [ ] **Step 6: Commit**

```bash
git add .github/workflows/ci.yml .gitignore
git commit -m "ci: add coverage job with line-coverage threshold gate (baseline <PCT>%)"
```

---

## Final verification

- [ ] Full suite green locally: `ctest --test-dir build -E bench_ --output-on-failure` → 79/79.
- [ ] New fuzz tests also green under ASan: `ctest --test-dir build-asan -R "fuzz_mcp_json|fuzz_checkpoint" --output-on-failure` → PASS (configure/build `build-asan` first if stale; CI's ASan matrix will run them, so close the loop locally before push).
- [ ] Formatting clean: `clang-format --dry-run --Werror tests/fuzz/fuzz_mcp_json.c tests/fuzz/fuzz_checkpoint.c` → no output.
- [ ] `git log --oneline -3` shows the three commits; working tree clean.
- [ ] Do NOT push (user decides push timing).
