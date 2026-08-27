# Autonomous Closed-Loop Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Aegis 现有模块连接成完整自主闭环（Goal→Context→Planner→Plan→TaskGraph→Scheduler→Executor→Tool→Observation→Critic→Reflection→Replanner→NewPlan），支持失败重规划、任务级/轮次级取消与超时、Checkpoint持久化与恢复，并交付可运行的 LLM 驱动 system test。

**Architecture:** 新增 `autonomous_agent` Orchestrator（`include/aegis/autonomous_agent.h` + `src/autonomous_agent.c`）位于 Plugin/Application 层，仅通过 `include/aegis/*.h` 公共接口编排 `planner/scheduler/executor/critic/replanner/memory/context/storage/checkpoint/cancellation`，主循环 `Context.build → Planner → DAG → inner(Scheduler.next → Executor.execute_with_timeout) → checkpoint → Critic → Reflection → Replanner`，任务级与轮次级取消协作式检查。

**Tech Stack:** C11, CMake, aegis_core (sqlite3), provider_llm_mock / provider_storage_sqlite, GTest, ASan/TSan

---

## File Structure

| 文件 | 职责 |
|------|------|
| `include/aegis/autonomous_agent.h` (新建) | 公共 API：config/result/create/destroy/run/cancel/checkpoint/restore |
| `src/autonomous_agent.c` (新建) | Orchestrator 实现：状态机、主循环、DAG 构建、checkpoint 双写、取消/超时透传 |
| `tests/system/test_autonomous_closed_loop.c` (新建) | 6 场景 system test：happy_path / retry_then_replan / timeout / cancellation / checkpoint_recovery / 边界 |
| `CMakeLists.txt` (修改) | `aegis_core` 新增源文件，`aegis_add_test` 注册 system test |
| `examples/autonomous_demo.c` (可选，不入核心验收) | 最小可运行演示 |

---

## Chunk 1: 公共 API 与骨架

### Task 1: 定义 autonomous_agent 头文件

**Files:**
- Create: `include/aegis/autonomous_agent.h`
- Test: 编译检查 `cmake -S . -B build && cmake --build build -j`

- [ ] **Step 1: 编写头文件**

```c
// include/aegis/autonomous_agent.h
#ifndef AEGIS_AUTONOMOUS_AGENT_H
#define AEGIS_AUTONOMOUS_AGENT_H
#include "aegis/types.h"
#include "aegis/status.h"
#include "aegis/cancellation.h"
#include "aegis/plan.h"
#include "aegis/graph.h"
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct aegis_autonomous_agent aegis_autonomous_agent_t;

typedef struct {
    const aegis_provider_registry_t* provider_registry; // borrowed, for planner/llm
    const char* llm_provider_name;                      // borrowed, copied
    aegis_memory_t*             memory;       // borrowed
    aegis_context_t*            context;      // borrowed
    aegis_storage_t*            storage;      // borrowed
    aegis_checkpoint_t*         checkpoint;   // borrowed
    aegis_cancellation_token_t* cancel_token; // borrowed, may be NULL (internal created)
    uint32_t max_iterations;                  // 0 => 5 default
    uint64_t default_task_timeout_ns;        // 0 => no timeout
    const char* checkpoint_path;             // borrowed, for file checkpoint
} aegis_autonomous_agent_config_t;

typedef struct {
    aegis_status_t final_status;
    uint32_t iterations;
    uint32_t tasks_executed;
    bool recovered_from_checkpoint;
} aegis_autonomous_result_t;

// planner/scheduler/executor/critic/replanner 由 Orchestrator 内部创建持有，
// 避免 config 过度膨胀；调用方只需提供上述 borrowed 依赖。

aegis_status_t aegis_autonomous_agent_create(aegis_autonomous_agent_t** out,
                                             const aegis_autonomous_agent_config_t* config);
void aegis_autonomous_agent_destroy(aegis_autonomous_agent_t* aa);

aegis_status_t aegis_autonomous_agent_run(aegis_autonomous_agent_t* aa,
                                          const char* goal_text,
                                          aegis_autonomous_result_t* out_result);
aegis_status_t aegis_autonomous_agent_cancel(aegis_autonomous_agent_t* aa);
aegis_status_t aegis_autonomous_agent_checkpoint_save(aegis_autonomous_agent_t* aa,
                                                      const char* path);
aegis_status_t aegis_autonomous_agent_restore(aegis_autonomous_agent_t* aa,
                                              const char* path);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: 验证编译**

Run: `cmake -S . -B build && cmake --build build -j 2>&1 | tail -20`
Expected: 编译通过（可能报 autonomous_agent.c 未实现，但头文件本身无错）

- [ ] **Step 3: Commit**

```bash
git add include/aegis/autonomous_agent.h
git commit -m "feat: add autonomous_agent public API"
```

---

### Task 2: 骨架实现与 CMake 集成

**Files:**
- Create: `src/autonomous_agent.c`
- Modify: `CMakeLists.txt:116-123` (aegis_core 源文件列表)

- [ ] **Step 1: 最小骨架**

```c
// src/autonomous_agent.c
#include "aegis/autonomous_agent.h"
#include <stdlib.h>
#include <string.h>

struct aegis_autonomous_agent {
    aegis_autonomous_agent_config_t cfg;
    char* llm_name_copy;
    aegis_planner_t* planner;
    aegis_scheduler_t* scheduler;
    aegis_executor_t* executor;
    aegis_critic_t* critic;
    // internal owned token if cfg.cancel_token == NULL
    aegis_cancellation_token_t* owned_token;
};

aegis_status_t aegis_autonomous_agent_create(aegis_autonomous_agent_t** out,
                                             const aegis_autonomous_agent_config_t* cfg) {
    if (!out || !cfg) return AEGIS_ERR_INVALID;
    // validate, copy llm_name, create planner/scheduler/executor/critic
    return AEGIS_OK;
}
void aegis_autonomous_agent_destroy(aegis_autonomous_agent_t* aa) { /* destroy owned */ }
aegis_status_t aegis_autonomous_agent_run(aegis_autonomous_agent_t* aa, const char* goal,
                                          aegis_autonomous_result_t* out) { return AEGIS_ERR_NOT_IMPLEMENTED; }
aegis_status_t aegis_autonomous_agent_cancel(aegis_autonomous_agent_t* aa) {
    if (!aa) return AEGIS_ERR_INVALID;
    aegis_cancellation_token_t* tok = aa->cfg.cancel_token ? aa->cfg.cancel_token : aa->owned_token;
    if (tok) aegis_cancellation_token_request_cancel(tok);
    return AEGIS_OK;
}
```

- [ ] **Step 2: CMake 注册**

在 `add_library(aegis_core ...)` 列表追加 `src/autonomous_agent.c`。

- [ ] **Step 3: 验证构建 0 warning**

Run: `cmake -S . -B build && cmake --build build -j 2>&1 | tail -20`
Expected: 0 warning

- [ ] **Step 4: Commit**

```bash
git add src/autonomous_agent.c CMakeLists.txt
git commit -m "feat: scaffold autonomous_agent with CMake integration"
```

---

## Chunk 2: 核心闭环（Context→Planner→Graph→Scheduler→Executor→Checkpoint）

### Task 3: 实现 create/destroy 与内部依赖初始化

**Files:**
- Modify: `src/autonomous_agent.c`

- [ ] **Step 1: 完善 create**

按现有公共接口初始化:
- `aegis_planner_create(&aa->planner, &(aegis_planner_config_t){.provider_registry=cfg->provider_registry, .llm_provider_name=copy})`
- `aegis_scheduler_create(&aa->scheduler)`
- `aegis_executor_create(&aa->executor, 2 workers)` // 查 executor.h 实际签名
- `aegis_critic_create(&aa->critic)`
- 若 cfg->cancel_token==NULL 则 `aegis_cancellation_token_create(&aa->owned_token)`
- 错误路径逐级 destroy 已创建对象，无泄漏

参考: `include/aegis/planner.h:29`, `scheduler.h:70`, `executor.h:90`, `critic.h:60`, `cancellation.h:40`

- [ ] **Step 2: 验证**

Run: `ctest --test-dir build -R unit --output-on-failure 2>&1 | tail -20`
Expected: 原有单测仍过

- [ ] **Step 3: Commit**

### Task 4: 实现 Plan→DAG→调度→执行→Checkpoint 内循环

**Files:**
- Modify: `src/autonomous_agent.c`

- [ ] **Step 1: 实现 run() 内循环骨架**

伪实现需走公共接口:
1. `aegis_context_build` (或 memory+goal 组装)
2. `aegis_planner_plan(planner, goal_text, token, &plan)`
3. `aegis_plan_validate(plan)` + `aegis_plan_materialize(plan, &graph)` // 若无 materialize 则手动遍历 `aegis_plan_step_count` + `aegis_plan_step_at` + `aegis_task_create` + `aegis_task_graph_add_dependency`
4. `aegis_scheduler_attach(scheduler, graph)`
5. `while ((task = aegis_scheduler_next(scheduler)) != NULL) {`
   - 检查 `aegis_cancellation_token_is_cancelled(token)`
   - 设置 `aegis_task_set_timeout(task, cfg.default_task_timeout_ns)` 若有
   - `aegis_executor_submit(executor, task, work_fn, user)`
   - `aegis_executor_wait(executor, task_id, &outcome)` // 阻塞等待
   - `aegis_scheduler_notify_complete(scheduler, task, outcome)`
   - `aegis_checkpoint_save(checkpoint, path, agent_state, goal, plan, graph)` 每 task 后
   - `}`
6. 轮次结束再 checkpoint 一次

`work_fn` 需调用 `aegis_tool_invoke` 或 mock tool（system test 中注册 mock tool）

- [ ] **Step 2: 编写最小 system test 桩验证编译**

Run: `cmake --build build -j && ./build/tests_system_autonomous 2>&1 | tail -20` (占位)

- [ ] **Step 3: Commit**

---

## Chunk 3: Critic/Reflection/Replanner 与重规划

### Task 5: 接入 Critic→Reflection→Replanner

**Files:**
- Modify: `src/autonomous_agent.c`

- [ ] **Step 1: 实现外循环的重规划分支**

```c
critique = aegis_critic_evaluate(critic, goal_text, plan, graph);
if (critique.result == AEGIS_CRITIQUE_SUCCESS) break;
if (critique.result == AEGIS_CRITIQUE_REPLAN_REQUIRED || had_failed) {
    aegis_reflection_create(&refl, graph);
    const char* feedback = aegis_reflection_feedback(refl);
    aegis_plan_t* new_plan = NULL;
    aegis_replan(planner, plan, feedback, token, &new_plan);
    aegis_plan_destroy(plan); plan = new_plan;
    aegis_reflection_destroy(refl);
    // 清理旧 graph, scheduler detach, 继续下一 iter
    continue;
}
```

需处理 `AEGIS_REFLECTION` 空图、version 递增 (`aegis_plan_set_version(new_plan, old_version+1)`)

- [ ] **Step 2: 重试与重规划协同**

可重试错误由 executor 内部处理（`aegis_task_retry_policy_t`），Orchestrator 仅在 `executor_wait` 返回 `FAILED` 且 `reflection.failed_count>0` 时才进入 replanner，避免重复重试

- [ ] **Step 3: 验证 happy_path + retry_then_replan 场景手动跑通**

Run: `cmake --build build && ctest -R autonomous --output-on-failure`

- [ ] **Step 4: Commit**

---

## Chunk 4: 取消/超时/Checkpoint 恢复

### Task 6: 取消与超时

**Files:**
- Modify: `src/autonomous_agent.c`

- [ ] **Step 1: 任务级超时**

在每次 `executor_submit` 前设置 `task.config.timeout_ns = cfg.default_task_timeout_ns`，executor 返回 `AEGIS_EXEC_TIMED_OUT` 时标记为需要重规划

- [ ] **Step 2: 轮次级取消**

`aegis_autonomous_agent_cancel` 原子置位 token；`run()` 的 inner_loop 每次 `next()` 前检查 `is_cancelled`，若置位则 `mark_cancelled_remaining` + `break`，外循环返回 `AEGIS_CANCELLED`

- [ ] **Step 3: Commit**

### Task 7: Checkpoint 双写与恢复

**Files:**
- Modify: `src/autonomous_agent.c`
- Relevant: `include/aegis/checkpoint.h:50-120`

- [ ] **Step 1: 实现 save/restore**

- save: `aegis_checkpoint_create` → `aegis_checkpoint_save_to_path(ckpt, path, ...)` 或 `aegis_checkpoint_serialize` + storage put
- restore: `aegis_checkpoint_load_from_path(path, &ckpt)` → 验证 `AEGIS_CHECKPOINT_OK` → 重建 `plan/graph/task_states` → 将 `RUNNING` 任务重置为 `PENDING` → `scheduler_attach`

- [ ] **Step 2: 处理 §13 恢复语义**

`partial execution / in-flight task / duplicate execution` 幂等：已 SUCCESS 的 task 跳过重放

- [ ] **Step 3: Commit**

---

## Chunk 5: System Test 与验收

### Task 8: 编写完整 system test

**Files:**
- Create: `tests/system/test_autonomous_closed_loop.c`
- Modify: `CMakeLists.txt` (aegis_add_test)

**Test 用例 (6 场景):**

```c
// 1. happy_path_llm_mock: mock 返回 3 步 Plan，全 SUCCESS → critic SUCCESS
// 2. retry_then_replan: Task2 第一次 TOOL_ERROR(transient) 重试耗尽→ replan→ 第二次 Plan 成功
// 3. timeout_path: default_task_timeout_ns=10ms, 构造慢 tool 超时→重规划
// 4. cancellation: 子线程 pthread_create 50ms 后 cancel → run 返回 AEGIS_CANCELLED
// 5. checkpoint_recovery: 执行 2/3 后 destroy(不删 db) → 新实例 restore → 从 Task3 续跑成功
// 6. 边界: NULL goal / 空 goal / max_iterations=1 / cycle检测
```

每个用例:
- 注册 `provider_llm_mock` (按调用次数返回不同 STEP DSL)
- 注册 mock tool (`aegis_tool_registry_register`)
- 使用 `provider_storage_sqlite` 临时 db (`build/tests/system/*.db`, 用例结束 unlink)
- 断言: 返回值 / checkpoint 文件存在 / 恢复后 task 状态 / memory 记录数

- [ ] **Step 1: 先写 failing test**

Run: `cmake --build build -j 2>&1 | tail -20` Expected: 编译失败（无实现）

- [ ] **Step 2: 实现使 test 通过**

Run: `ctest --test-dir build --output-on-failure 2>&1 | tail -40`
Expected: 6/6 PASS

- [ ] **Step 3: ASan/TSan**

Run: `cmake -S . -B build-asan -DAEGIS_ENABLE_ASAN=ON && cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure`
Expected: 0 泄漏/告警

- [ ] **Step 4: Commit**

```bash
git add tests/system/test_autonomous_closed_loop.c CMakeLists.txt
git commit -m "test: add system autonomous closed-loop coverage"
```

---

## Chunk 6: 收尾与 DoD

### Task 9: 全量验收

- [ ] **Step 1: 构建 0 warning**

Run: `cmake -S . -B build && cmake --build build -j 2>&1 | grep -i warning || echo "0 warnings"`

- [ ] **Step 2: 全量测试**

Run: `ctest --test-dir build --output-on-failure`

- [ ] **Step 3: git diff 检查无无关改动**

Run: `git diff --stat HEAD~3`

- [ ] **Step 4: 生成完成报告**

按 AGENTS.md §20 输出 Summary/Affected Modules/API/Tests/Risks

---

## 依赖与风险

- `aegis_planner_plan` 的 STEP DSL 格式需严格遵守 `STEP|<id>|<type>|<deps>|<name>|<desc>`，mock 需按此生成
- `aegis_executor` 的 worker 数与 `aegis_task_retry_policy_t` 需在 plan step spec 中正确透传
- checkpoint 序列化若不支持 plan_version，需在 `src/checkpoint.c` 最小扩展（保持 ABI compatible，新增字段可空）

## 验收标准

- [ ] 构建 0 warning
- [ ] 44 原有单测 + 1 system test(6场景) 全过
- [ ] ASan 0 泄漏，TSan 无告警
- [ ] git diff 仅新增 autonomous_agent + system test + CMake，无无关重构
- [ ] 无 TODO/假成功/吞错
