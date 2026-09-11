/**
 * @file plan.c
 * @brief Versioned structured plan: construction, validation,
 *        task-graph materialization and serialization.
 */
#include "planner_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

/**
 * @brief Create an empty versioned plan for a goal.
 *
 * The goal string is duplicated and the plan starts at version 1 with no
 * steps. Empty/NULL goals are rejected.
 *
 * @param[out] out   Receives the new plan on success; set only on success.
 * @param[in]  goal  Non-empty goal description (copied).
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad arguments,
 *         AEGIS_ERR_NOMEM on allocation failure.
 *
 * Ownership: the caller owns the returned plan and must release it with
 * aegis_plan_destroy().
 */
aegis_status_t aegis_plan_create(aegis_plan_t** out, const char* goal)
{
    if (!out || !goal || goal[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    aegis_plan_t* plan = calloc(1, sizeof(*plan));
    if (!plan) {
        return AEGIS_ERR_NOMEM;
    }
    plan->goal = strdup(goal);
    if (!plan->goal) {
        free(plan);
        return AEGIS_ERR_NOMEM;
    }
    plan->version = 1u;
    *out          = plan;
    return AEGIS_OK;
}

/**
 * @brief Destroy a plan with all step names, descriptions, tool names, and inputs.
 *
 * NULL is accepted and ignored.
 *
 * @param[in] plan  Plan to destroy, or NULL.
 */
void aegis_plan_destroy(aegis_plan_t* plan)
{
    if (!plan) {
        return;
    }
    for (size_t i = 0; i < plan->step_count; i++) {
        free(plan->steps[i].name);
        free(plan->steps[i].desc);
        free(plan->steps[i].tool_name);
        free(plan->steps[i].input);
    }
    free(plan->steps);
    free(plan->goal);
    free(plan);
}

/* ── Accessors ─────────────────────────────────────────────────────────────── */

/**
 * @brief Borrow the plan's goal string.
 *
 * @param[in] plan  Plan, or NULL.
 * @return Pointer to the goal text, or NULL for NULL. Valid until destruction.
 */
const char* aegis_plan_goal(const aegis_plan_t* plan)
{
    return plan ? plan->goal : NULL;
}

/**
 * @brief Return the plan's schema version (starts at 1).
 *
 * @param[in] plan  Plan, or NULL.
 * @return Version number, or 0 for NULL.
 */
uint32_t aegis_plan_version(const aegis_plan_t* plan)
{
    return plan ? plan->version : 0u;
}

/**
 * @brief Overwrite the plan's schema version stamp.
 *
 * Used when migrating or reinterpreting a plan under a newer schema.
 * NULL plan is ignored.
 *
 * @param[in] plan     Plan to stamp.
 * @param[in] version  New version number.
 */
void aegis_plan_set_version(aegis_plan_t* plan, uint32_t version)
{
    if (plan) {
        plan->version = version;
    }
}

/**
 * @brief Return the number of steps currently in the plan.
 *
 * @param[in] plan  Plan, or NULL.
 * @return Step count, or 0 for NULL.
 */
size_t aegis_plan_step_count(const aegis_plan_t* plan)
{
    return plan ? plan->step_count : 0u;
}

/**
 * @brief Find a step by its plan-scoped id.
 *
 * @param[in] plan  Plan to search, or NULL.
 * @param[in] id    Step id to look up.
 * @return Pointer to the step, or NULL when absent or @p plan is NULL.
 *         The pointer borrows plan storage; valid until the plan is mutated
 *         (growth may reallocate) or destroyed.
 */
const aegis_plan_step_t* aegis_plan_find_step(const aegis_plan_t* plan, int64_t id)
{
    if (!plan) {
        return NULL;
    }
    for (size_t i = 0; i < plan->step_count; i++) {
        if (plan->steps[i].id == id) {
            return &plan->steps[i];
        }
    }
    return NULL;
}

/**
 * @brief Return how many dependencies a step declares.
 *
 * @param[in] plan     Plan holding the step, or NULL.
 * @param[in] step_id  Step id to inspect.
 * @return Dependency count, or 0 when the step (or plan) is absent.
 */
size_t aegis_plan_step_dep_count(const aegis_plan_t* plan, int64_t step_id)
{
    const aegis_plan_step_t* st = aegis_plan_find_step(plan, step_id);
    return st ? st->dep_count : 0u;
}

/* ── Construction ──────────────────────────────────────────────────────────── */

/**
 * @brief Compute the smallest non-negative step id not yet used.
 *
 * Scans upward from 0 so ids stay dense after removals.
 *
 * @param[in] plan  Plan to scan (NULL yields 0 via the public wrapper's lookup miss).
 * @return Smallest free step id.
 */
static int64_t next_free_id(const aegis_plan_t* plan)
{
    int64_t id = 0;
    while (aegis_plan_find_step(plan, id) != NULL) {
        id++;
    }
    return id;
}

/**
 * @brief Return the smallest non-negative step id not yet used in the plan.
 *
 * Convenience wrapper for auto-assigning step ids.
 *
 * @param[in] plan  Plan to scan.
 * @return Smallest free step id.
 */
int64_t aegis_plan_next_free_id(const aegis_plan_t* plan)
{
    return next_free_id(plan);
}

/**
 * @brief Append a step to the plan, taking ownership of heap arguments.
 *
 * The @p name/@p desc buffers are adopted on success; on ANY failure they
 * are freed here, so callers must never touch them after the call. The tool
 * name and input payload are duplicated/copied internally. Dependencies must
 * reference existing steps (no self-deps or forward references) and stay
 * within AEGIS_PLAN_MAX_DEPS; the plan itself caps at AEGIS_PLAN_MAX_STEPS.
 *
 * @param[in] plan       Plan to extend.
 * @param[in] id         Caller-chosen step id (uniqueness checked by the caller).
 * @param[in] name       Heap name string; consumed (owned or freed) always.
 * @param[in] desc       Heap description string, may be NULL; consumed always.
 * @param[in] type       Task type carried into materialization.
 * @param[in] priority   Scheduling priority carried into materialization.
 * @param[in] timeout_ms Timeout carried into materialization.
 * @param[in] retry      Retry policy carried into materialization.
 * @param[in] tool_name  Optional tool name (copied), may be NULL.
 * @param[in] input      Optional input payload (copied), may be NULL when len is 0.
 * @param[in] input_len  Input payload length in bytes.
 * @param[in] deps       Array of @p dep_count existing step ids, may be NULL when 0.
 * @param[in] dep_count  Number of dependency ids.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad arguments/limits/
 *         dangling deps, AEGIS_ERR_NOMEM on allocation failure.
 */
aegis_status_t aegis_planner_add_step_owned(aegis_plan_t* plan, int64_t id, char* name, char* desc,
                                            aegis_task_type_t type, int priority, long timeout_ms,
                                            aegis_task_retry_policy_t retry, const char* tool_name,
                                            const void* input, size_t input_len,
                                            const int64_t* deps, size_t dep_count)
{
    if (!plan || !name || name[0] == '\0' || dep_count > AEGIS_PLAN_MAX_DEPS ||
        plan->step_count >= AEGIS_PLAN_MAX_STEPS) {
        free(name);
        free(desc);
        return AEGIS_ERR_INVALID;
    }

    char*              tool_copy  = NULL;
    void*              input_copy = NULL;
    aegis_plan_step_t* slot       = NULL;

    if (tool_name) {
        tool_copy = strdup(tool_name);
        if (!tool_copy) {
            goto fail_nomem;
        }
    }
    if (input_len > 0) {
        if (!input) {
            goto fail_invalid;
        }
        input_copy = malloc(input_len);
        if (!input_copy) {
            goto fail_nomem;
        }
        memcpy(input_copy, input, input_len);
    }
    for (size_t i = 0; i < dep_count; i++) {
        if (deps[i] == id || !aegis_plan_find_step(plan, deps[i])) {
            goto fail_invalid; /* self-dep or forward/unknown reference */
        }
    }

    if (plan->step_count == plan->step_cap) {
        size_t             cap   = plan->step_cap == 0 ? 8 : plan->step_cap * 2;
        aegis_plan_step_t* grown = realloc(plan->steps, cap * sizeof(*grown));
        if (!grown) {
            goto fail_nomem;
        }
        plan->steps    = grown;
        plan->step_cap = cap;
    }

    slot = &plan->steps[plan->step_count];
    memset(slot, 0, sizeof(*slot));
    slot->id         = id;
    slot->name       = name; /* ownership moved in */
    slot->desc       = desc;
    slot->type       = type;
    slot->priority   = priority;
    slot->timeout_ms = timeout_ms;
    slot->retry      = retry;
    slot->tool_name  = tool_copy;
    slot->input      = input_copy;
    slot->input_len  = input_len;
    slot->dep_count  = dep_count;
    if (dep_count > 0) {
        memcpy(slot->deps, deps, dep_count * sizeof(deps[0]));
    }
    plan->step_count++;
    return AEGIS_OK;

fail_invalid:
    free(tool_copy);
    free(input_copy);
    free(name);
    free(desc);
    return AEGIS_ERR_INVALID;

fail_nomem:
    free(tool_copy);
    free(input_copy);
    free(name);
    free(desc);
    return AEGIS_ERR_NOMEM;
}

/**
 * @brief Append a step described by a spec; all strings/payloads are copied.
 *
 * Unlike aegis_planner_add_step_owned(), the caller retains ownership of
 * everything in @p spec. AEGIS_PLAN_STEP_ID_AUTO assigns the smallest free
 * id; an explicit id colliding with an existing step reports AEGIS_ERR_BUSY.
 *
 * @param[in]  plan    Plan to extend.
 * @param[in]  spec    Step specification (name required, non-empty).
 * @param[out] out_id  Optional receiver for the assigned step id, may be NULL.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad arguments,
 *         AEGIS_ERR_BUSY for a duplicate explicit id, AEGIS_ERR_NOMEM on
 *         allocation failure.
 */
aegis_status_t aegis_plan_add_step(aegis_plan_t* plan, const aegis_plan_step_spec_t* spec,
                                   int64_t* out_id)
{
    if (!plan || !spec || !spec->name || spec->name[0] == '\0') {
        return AEGIS_ERR_INVALID;
    }
    if (spec->step_id != AEGIS_PLAN_STEP_ID_AUTO && spec->step_id < 0) {
        return AEGIS_ERR_INVALID;
    }
    if (spec->dep_count > AEGIS_PLAN_MAX_DEPS || (spec->dep_count > 0 && !spec->deps)) {
        return AEGIS_ERR_INVALID;
    }

    const int64_t id =
        (spec->step_id == AEGIS_PLAN_STEP_ID_AUTO) ? next_free_id(plan) : spec->step_id;
    if (aegis_plan_find_step(plan, id) != NULL) {
        return AEGIS_ERR_BUSY; /* duplicate explicit id */
    }

    char* name_copy = strdup(spec->name);
    char* desc_copy = spec->desc ? strdup(spec->desc) : NULL;
    if (!name_copy || (spec->desc && !desc_copy)) {
        free(name_copy);
        free(desc_copy);
        return AEGIS_ERR_NOMEM;
    }

    aegis_status_t rc = aegis_planner_add_step_owned(
        plan, id, name_copy, desc_copy, spec->type, spec->priority, spec->timeout_ms, spec->retry,
        spec->tool_name, spec->input, spec->input_len, spec->deps, spec->dep_count);
    /* On failure add_step_owned consumed the copies; nothing more to release. */
    if (rc == AEGIS_OK && out_id) {
        *out_id = id;
    }
    return rc;
}

/* ── Validation ────────────────────────────────────────────────────────────── */

/**
 * @brief Validate a plan's structural invariants.
 *
 * Checks that the plan is non-empty with all steps named, every dependency
 * references an existing step exactly once (no self/duplicate/dangling
 * deps), and the dependency graph is acyclic (iterative three-color DFS).
 *
 * @param[in] plan  Plan to validate.
 * @return AEGIS_OK when valid, AEGIS_ERR_INVALID for any violation,
 *         AEGIS_ERR_NOMEM when the DFS work buffers cannot be allocated.
 */
aegis_status_t aegis_plan_validate(const aegis_plan_t* plan)
{
    if (!plan || plan->step_count == 0) {
        return AEGIS_ERR_INVALID;
    }
    for (size_t i = 0; i < plan->step_count; i++) {
        const aegis_plan_step_t* s = &plan->steps[i];
        if (!s->name || s->name[0] == '\0') {
            return AEGIS_ERR_INVALID;
        }
        for (size_t d = 0; d < s->dep_count; d++) {
            if (s->deps[d] == s->id || !aegis_plan_find_step(plan, s->deps[d])) {
                return AEGIS_ERR_INVALID;
            }
            for (size_t p = 0; p < d; p++) {
                if (s->deps[p] == s->deps[d]) {
                    return AEGIS_ERR_INVALID; /* duplicate dependency */
                }
            }
        }
    }

    /* Cycle detection: three-color DFS over the dependency edges.
     * color 0 = unvisited, 1 = on stack, 2 = done. */
    uint8_t* color = calloc(plan->step_count, sizeof(*color));
    int64_t* stack = malloc(plan->step_count * sizeof(*stack));
    if (!color || !stack) {
        free(color);
        free(stack);
        return AEGIS_ERR_NOMEM;
    }

    aegis_status_t rc = AEGIS_OK;
    for (size_t root = 0; root < plan->step_count && rc == AEGIS_OK; root++) {
        if (color[root] != 0) {
            continue;
        }
        size_t top   = 0;
        stack[top++] = (int64_t)root;
        color[root]  = 1;
        while (top > 0 && rc == AEGIS_OK) {
            size_t             idx       = (size_t)stack[top - 1];
            aegis_plan_step_t* s         = &plan->steps[idx];
            bool               descended = false;
            for (size_t d = 0; d < s->dep_count; d++) {
                /* Deps always reference existing steps; map id -> index. */
                size_t di = plan->step_count;
                for (size_t k = 0; k < plan->step_count; k++) {
                    if (plan->steps[k].id == s->deps[d]) {
                        di = k;
                        break;
                    }
                }
                if (di == plan->step_count) {
                    rc = AEGIS_ERR_INVALID;
                    break;
                }
                if (color[di] == 1) {
                    rc = AEGIS_ERR_INVALID; /* back-edge -> cycle */
                    break;
                }
                if (color[di] == 0) {
                    stack[top++] = (int64_t)di;
                    color[di]    = 1;
                    descended    = true;
                    break;
                }
            }
            if (rc == AEGIS_OK && !descended) {
                color[idx] = 2;
                top--;
            }
        }
    }
    free(color);
    free(stack);
    return rc;
}

/* ── Materialization ───────────────────────────────────────────────────────── */

/**
 * @brief Materialize a validated plan into an executable task graph.
 *
 * Validates first, then creates one task per step (copying type, priority,
 * timeout, retry policy, tool name as "tool" metadata, and input payload)
 * and wires the step dependencies as graph edges, with a final graph
 * validation. Any failure destroys the partial graph, so no half-built
 * graph ever escapes.
 *
 * @param[in]  plan  Plan to materialize (must validate cleanly).
 * @param[out] out   Receives the new task graph; set only on success.
 * @return AEGIS_OK on success, or the validation/creation error otherwise.
 *
 * Ownership: the caller owns the returned graph and must release it with
 * aegis_task_graph_destroy().
 */
aegis_status_t aegis_plan_materialize(const aegis_plan_t* plan, aegis_task_graph_t** out)
{
    if (!out) {
        return AEGIS_ERR_INVALID;
    }
    *out              = NULL;
    aegis_status_t rc = aegis_plan_validate(plan);
    if (rc != AEGIS_OK) {
        return rc;
    }

    aegis_task_graph_t* graph = NULL;
    rc                        = aegis_task_graph_create(&graph);
    if (rc != AEGIS_OK) {
        return rc;
    }

    /* Map plan steps to created tasks positionally. */
    aegis_task_t** tasks = calloc(plan->step_count, sizeof(*tasks));
    if (!tasks) {
        aegis_task_graph_destroy(graph);
        return AEGIS_ERR_NOMEM;
    }

    rc = AEGIS_OK;
    for (size_t i = 0; i < plan->step_count && rc == AEGIS_OK; i++) {
        const aegis_plan_step_t* s    = &plan->steps[i];
        aegis_task_t*            task = NULL;
        rc                            = aegis_task_create(&task, s->name, s->desc);
        if (rc != AEGIS_OK) {
            break;
        }
        aegis_task_set_type(task, s->type);
        aegis_task_set_priority(task, s->priority);
        aegis_task_set_timeout_ms(task, s->timeout_ms);
        aegis_task_set_retry_policy(task, s->retry);
        if (s->tool_name) {
            aegis_task_set_metadata(task, "tool", s->tool_name);
        }
        if (s->input_len > 0) {
            rc = aegis_task_set_input(task, s->input, s->input_len);
            if (rc != AEGIS_OK) {
                aegis_task_destroy(task); /* graph never took ownership */
                break;
            }
        }
        rc = aegis_task_graph_add_task(graph, task); /* graph owns task now */
        if (rc != AEGIS_OK) {
            aegis_task_destroy(task);
            break;
        }
        tasks[i] = task;
    }

    if (rc == AEGIS_OK) {
        for (size_t i = 0; i < plan->step_count && rc == AEGIS_OK; i++) {
            const aegis_plan_step_t* s = &plan->steps[i];
            for (size_t d = 0; d < s->dep_count && rc == AEGIS_OK; d++) {
                const aegis_plan_step_t* src = aegis_plan_find_step(plan, s->deps[d]);
                size_t                   si  = plan->step_count;
                for (size_t k = 0; k < plan->step_count; k++) {
                    if (&plan->steps[k] == src) {
                        si = k;
                        break;
                    }
                }
                rc = aegis_task_graph_add_dependency(graph, tasks[si], tasks[i]);
            }
        }
    }

    if (rc == AEGIS_OK) {
        rc = aegis_task_graph_validate(graph);
    }

    if (rc != AEGIS_OK) {
        aegis_task_graph_destroy(graph); /* destroys all added tasks */
        free(tasks);
        return rc;
    }

    free(tasks);
    *out = graph;
    return AEGIS_OK;
}

/* ── Serialization ─────────────────────────────────────────────────────────── */

/**
 * @brief Serialize a plan to a line-based text form ("PLAN|v" + "STEP|..." lines).
 *
 * Each step line records id, type name, comma-separated dependency ids, name,
 * and description. The buffer is pre-sized with a worst-case upper bound, so
 * an overflow can only signal an internal accounting bug (AEGIS_ERR_INTERNAL).
 *
 * @param[in]  plan     Plan to serialize.
 * @param[out] out_str  Receives the NUL-terminated text; set only on success.
 * @return AEGIS_OK on success, AEGIS_ERR_INVALID for bad arguments,
 *         AEGIS_ERR_NOMEM on allocation failure, AEGIS_ERR_INTERNAL on
 *         buffer-accounting overflow.
 *
 * Ownership: the caller owns the returned string and must free() it.
 */
aegis_status_t aegis_plan_serialize(const aegis_plan_t* plan, char** out_str)
{
    if (!plan || !out_str) {
        return AEGIS_ERR_INVALID;
    }
    *out_str = NULL;

    /* Upper bound: header + worst-case per-step line. */
    size_t cap = 32;
    for (size_t i = 0; i < plan->step_count; i++) {
        const aegis_plan_step_t* s = &plan->steps[i];
        cap += strlen(s->name) + (s->desc ? strlen(s->desc) : 0) + 64 + s->dep_count * 12;
    }
    char* buf = malloc(cap);
    if (!buf) {
        return AEGIS_ERR_NOMEM;
    }
    size_t off = 0;
    int    n   = snprintf(buf, cap, "PLAN|%u\n", (unsigned)plan->version);
    if (n < 0) {
        free(buf);
        return AEGIS_ERR_INTERNAL;
    }
    off = (size_t)n;

    for (size_t i = 0; i < plan->step_count; i++) {
        static const char*       type_names[] = {"computational", "io",        "network", "shell",
                                                 "tool",          "provision", "sync",    "custom"};
        const aegis_plan_step_t* s            = &plan->steps[i];
        n = snprintf(buf + off, cap - off, "STEP|%lld|%s|", (long long)s->id,
                     type_names[(int)s->type]);
        if (n < 0 || (size_t)n >= cap - off) {
            free(buf);
            return AEGIS_ERR_INTERNAL;
        }
        off += (size_t)n;
        for (size_t d = 0; d < s->dep_count; d++) {
            n = snprintf(buf + off, cap - off, "%s%lld", d == 0 ? "" : ",", (long long)s->deps[d]);
            if (n < 0 || (size_t)n >= cap - off) {
                free(buf);
                return AEGIS_ERR_INTERNAL;
            }
            off += (size_t)n;
        }
        n = snprintf(buf + off, cap - off, "|%s|%s\n", s->name, s->desc ? s->desc : "");
        if (n < 0 || (size_t)n >= cap - off) {
            free(buf);
            return AEGIS_ERR_INTERNAL;
        }
        off += (size_t)n;
    }

    *out_str = buf;
    return AEGIS_OK;
}
