/**
 * @file agent_internal.h
 * @brief Internal agent struct layout.
 *
 * NOT part of the public API. Included by agent.c only.
 */
#ifndef AEGIS_AGENT_INTERNAL_H
#define AEGIS_AGENT_INTERNAL_H

#include "aegis/agent.h"
#include "aegis/event.h"
#include "aegis/common/mutex.h"
#include "aegis/common/atomic.h"
#include <stdbool.h>

/** Maximum goal string length. */
#define AEGIS_AGENT_GOAL_MAX 4096

/** Internal agent structure. */
struct aegis_agent {
    /* Identity */
    char* name;                       /**< Owned. */
    char  goal[AEGIS_AGENT_GOAL_MAX]; /**< Owned (stack-allocated buffer). */

    /* State machine */
    aegis_agent_state_t state;     /**< Protected by lock. */
    aegis_atomic_int_t* done_flag; /**< Atomic set when agent reaches terminal state. */

    /* Concurrency */
    aegis_mutex_t* lock; /**< Guards state and goal transitions. */

    /* Event bus */
    aegis_event_bus_t* bus; /**< Owned. */

    /* Synchronization for join() */
    aegis_atomic_int_t* joined; /**< Set when join() completes. */
};

#endif /* AEGIS_AGENT_INTERNAL_H */
