# Autonomous Closed-Loop 设计文档

- 日期: 2026-08-27
- 状态: Draft (待 Spec Review)
- 作者: Sisyphus
- 关联需求: 将现有模块串成完整自主闭环 Goal→Context→Planner→Plan→TaskGraph→Scheduler→Executor→Tool→Observation→Critic→Reflection→Replanner→NewPlan，满足 10 项约束

## 1. 背景与目标

Aegis 已具备 `Goal / Plan / TaskGraph / Scheduler / Executor / Tool / Critic / Reflection / Replanner / Context / Memory / Provider / Checkpoint / Cancellation` 等独立模块，但缺少将其按 AGENTS.md §7/8/13/14 约束串联的 **Orchestrator**。本设计新增 `autonomous_agent` 模块，仅通过 `include/aegis/*.h` 公共接口组合现有能力，不重写已有模块，保持架构边界，提供完整可运行的 LLM 驱动自主任务示例。

示例任务类型: **C - LLM 驱动**（`provider_llm_mock` 模拟 LLM 生成 Plan，验证完整数据流）。

## 2. 10 项约束映射

| 约束 | 设计满足 |
|------|----------|
| 1 不重写已有模块 | 仅新增 `src/autonomous_agent.c + include/aegis/autonomous_agent.h`，其余复用 |
| 2 公共接口集成 | 仅调用 `include/aegis/*.h`，不访问 `src/internal/*` |
| 3 保持模块边界 | 依赖方向 Foundation→Core→Planner/Executor/Memory→Provider/Tool/Storage，Orchestrator 位于 Plugin/Application 层 |
| 4 失败后 replan | Critic→Reflection→Replanner 闭环，支持 max_iterations |
| 5 cancellation | 任务级 + 轮次级协作式取消 via `aegis_cancellation_token_t` |
| 6 timeout | 任务级 `aegis_task_config.timeout_ns` 透传至 Executor |
| 7 checkpoint | 每 task 完成 + 每轮结束双写，覆盖 §13 七要素 |
| 8 恢复后继续 | `restore(checkpoint_id)` 重建 graph/scheduler/memory 引用，in-flight→pending |
| 9 system test | `tests/system/test_autonomous_closed_loop.c` 覆盖 6 场景 |
| 10 无临时 hack | 无 TODO/假成功/吞错，所有错误显式处理 |

## 3. 架构

```
Goal ─┐
      ├─► Context (aegis_context_build) ─► Planner (aegis_planner_create_plan)
      │         ▲                                 │ provider_llm_mock
Memory┘         │                                 ▼
            Reflection                      Plan ─► aegis_task_graph_t (DAG, cycle检测)
                                                    │
                                                    ▼
                                              Scheduler (aegis_scheduler_*)
                                                    │ next_ready_task (依赖/资源/优先级)
                                                    ▼
                                              Executor (aegis_executor_execute_task)
                                                    │ tool_registry + tool.execute
                                                    │ timeout_ns / retry / cancellation_token
                                                    ▼
                                              Observation ─► checkpoint (storage_sqlite)
                                                    │
                                                    ▼
                                          Critic (aegis_critic_evaluate)
                                                    │
                                          Reflection (aegis_reflection_summarize)
                                                    │
                                          Replanner (aegis_replanner_replan) ─► New Plan
                                                                                    │
                                                     ◄──────── 循环至 Plan ──────────┘
```

线程模型: Orchestrator 主线程驱动 inner_loop，`cancel()` 可并发原子置位 token。

## 4. 组件与公共 API

### 4.1 头文件

`include/aegis/autonomous_agent.h`

```c
typedef struct aegis_autonomous_agent aegis_autonomous_agent_t;
typedef struct {
    aegis_planner_t            *planner;      // borrowed
    aegis_scheduler_t          *scheduler;    // borrowed
    aegis_executor_t           *executor;     // borrowed
    aegis_critic_t             *critic;       // borrowed
    aegis_replanner_t          *replanner;    // borrowed
    aegis_memory_t             *memory;       // borrowed
    aegis_context_t            *context;      // borrowed
    aegis_storage_t            *storage;      // borrowed
    aegis_checkpoint_t         *checkpoint;   // borrowed
    aegis_cancellation_token_t *cancel_token; // borrowed
    uint32_t max_iterations;
    uint64_t default_task_timeout_ns; // 0 = 不设
} aegis_autonomous_agent_config_t;

typedef struct {
    aegis_result_t final_result;
    uint32_t iterations;
    uint32_t tasks_executed;
    bool recovered_from_checkpoint;
} aegis_autonomous_result_t;

aegis_autonomous_agent_t *aegis_autonomous_agent_create(const aegis_autonomous_agent_config_t *config);
void                       aegis_autonomous_agent_destroy(aegis_autonomous_agent_t *aa);
aegis_result_t aegis_autonomous_agent_run(aegis_autonomous_agent_t *aa,
                                          const aegis_goal_t *goal,
                                          aegis_autonomous_result_t *out_result);
aegis_result_t aegis_autonomous_agent_cancel(aegis_autonomous_agent_t *aa);
aegis_result_t aegis_autonomous_agent_checkpoint(aegis_autonomous_agent_t *aa);
aegis_result_t aegis_autonomous_agent_restore(aegis_autonomous_agent_t *aa,
                                              const char *checkpoint_id);
```

### 4.2 Ownership 与生命周期

- config 内所有指针 `borrowed`，调用方保证生命周期长于 aa
- `create` 深拷贝 config，不接管释放
- `destroy` 仅释放自身状态
- 同一 aa 不支持并发 run，cancel 可并发

### 4.3 错误码

透传 `AEGIS_*`，新增 `AEGIS_CANCELLED / AEGIS_TIMEOUT / AEGIS_MAX_ITERATIONS` 复用已有枚举。

## 5. 数据流与控制流

```c
context = aegis_context_build(memory, goal, checkpoint_refs);
plan = planner.create_plan(goal, context);
for (iter = 0; iter < max_iterations; iter++) {
  if (cancel_token.is_cancelled) return AEGIS_CANCELLED;
  graph = build_dag(plan); // add_dependency + cycle检测
  scheduler.attach(graph);
  while ((task = scheduler.next_ready()) != NULL) {
    if (cancel_token.is_cancelled) { mark_cancelled_remaining(); break; }
    task.config.timeout_ns = default_task_timeout_ns;
    rc = executor.execute_task(task, cancel_token);
    memory.record(rc.result);
    scheduler.mark_done/failed(task, rc);
    checkpoint.save(agent_state, goal, plan, graph, task_states, retry_state, memory_refs);
    if (rc == AEGIS_TIMEOUT || rc == AEGIS_CANCELLED) break;
  }
  checkpoint.save(...);
  cr = critic.evaluate(goal, plan, observations);
  if (cr == AEGIS_CRITIC_SUCCESS) return SUCCESS;
  if (cr == AEGIS_CRITIC_REPLAN || had_unretryable_failure) {
    reflection = reflection.summarize(goal, plan, observations, cr);
    plan = replanner.replan(goal, plan, reflection, context);
    if (!plan) return AEGIS_INTERNAL;
    continue;
  }
  return cr;
}
return AEGIS_MAX_ITERATIONS;
```

约束: Planner/Scheduler/Executor/Critic/Replanner 不直接互调，仅经 Orchestrator 编排。超时透传，取消协作式检查。

## 6. 重规划策略

- 可重试错误（`aegis_task_is_retryable`）先走 Executor retry/backoff
- 重试耗尽或不可重试失败 → Critic 返回 REPLAN → Reflection 汇总 → Replanner 产生新 Plan
- `provider_llm_mock` 按调用次数返回不同 Plan（首次 3 任务含 1 失败，二次修正）以构造确定性测试

## 7. Checkpoint / 恢复 / 错误与并发

- **覆盖** (§13): agent_state, goal, plan, plan_version, task_graph, task_states, retry_state, memory_references
- **持久化**: `aegis_storage_t` + `provider_storage_sqlite` 至临时 `.db`，`checkpoint_id = goal_id + iter`
- **恢复**: `storage.load → checkpoint.deserialize → 重建 graph/scheduler → in-flight→pending`，幂等跳过已完成 task
- **错误分类** (§14): INVALID/PERMISSION_DENIED/CANCELLED 不重试；TIMEOUT/BUSY/PROVIDER_TEMPORARY/TOOL_ERROR(transient) 可重试；OOM/INTERNAL 立即失败
- **并发**: 共享状态仅 `aa.state/iter/cancel_token`，锁顺序 `aa.mutex → scheduler.mutex → storage.mutex`，无新增全局可变状态
- **资源**: 覆盖正常/错误/取消/超时/初始化失败路径，复用各模块 destroy 语义

## 8. 示例与 System Test

### 8.1 运行示例

`examples/autonomous_demo.c` (可选) 演示 Goal="用 mock LLM 完成三步任务" 的完整 run。

### 8.2 System Test

`tests/system/test_autonomous_closed_loop.c`:

1. happy_path_llm_mock
2. retry_then_replan
3. timeout_path
4. cancellation (子线程 50ms 后 cancel)
5. checkpoint_recovery (执行 2/3 后 crash→restore→续跑)
6. 边界: 空 Goal / NULL / max_iterations / cycle检测

约定: 仅 mock，无网络；临时 db 在 `build/tests/system/*.db`，用例结束 unlink；ASan/TSan 零告警。

## 9. 构建与集成

- `aegis_core` 新增 `src/autonomous_agent.c`
- `CMakeLists.txt` 追加 `aegis_add_test(system_autonomous_closed_loop tests/system/test_autonomous_closed_loop.c)`
- 公共头安装至 `include/aegis/autonomous_agent.h`

## 10. 风险与未决

- provider_llm_mock 的 Plan 生成规则需与现有 `aegis_planner` 接口对齐，需在实现前确认 `aegis_planner_create_plan` 的入参/出参细节
- checkpoint 序列化格式复用现有 `aegis_checkpoint_t`，若其不支持 plan_version 需最小扩展（保持 ABI 兼容）
- in-flight 幂等依赖 Executor 对重复 task 的去重语义，需验证

## 11. 验收标准

- [ ] `cmake -S . -B build && cmake --build build` 0 warning
- [ ] `ctest --test-dir build --output-on-failure` 全过，含新增 system test 6 场景
- [ ] `cmake -S . -B build -DAEGIS_ENABLE_ASAN=ON` 零泄漏
- [ ] git diff 仅新增 autonomous_agent + system test + 必要的 CMake/头文件导出，无无关重构
