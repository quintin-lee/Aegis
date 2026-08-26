/**
 * @file plan.c
 * @brief Versioned structured plan: construction, validation,
 *        task-graph materialization and serialization.
 */
#include "internal/planner_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

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

const char* aegis_plan_goal(const aegis_plan_t* plan)
{
    return plan ? plan->goal : NULL;
}

uint32_t aegis_plan_version(const aegis_plan_t* plan)
{
    return plan ? plan->version : 0u;
}

void aegis_plan_set_version(aegis_plan_t* plan, uint32_t version)
{
    if (plan) {
        plan->version = version;
    }
}

size_t aegis_plan_step_count(const aegis_plan_t* plan)
{
    return plan ? plan->step_count : 0u;
}

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

size_t aegis_plan_step_dep_count(const aegis_plan_t* plan, int64_t step_id)
{
    const aegis_plan_step_t* st = aegis_plan_find_step(plan, step_id);
    return st ? st->dep_count : 0u;
}

/* ── Construction ──────────────────────────────────────────────────────────── */

static int64_t next_free_id(const aegis_plan_t* plan)
{
    int64_t id = 0;
    while (aegis_plan_find_step(plan, id) != NULL) {
        id++;
    }
    return id;
}

int64_t aegis_plan_next_free_id(const aegis_plan_t* plan)
{
    return next_free_id(plan);
}

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
