#pragma once

#include "atomic.h"
#include <stdbool.h>

#define MAX_THREADS (sizeof(ULONG_PTR) * 8) // Max threads is the number of bits in an affinity mask
#define MAX_NUMA_NODES 16
#define DEQUE_CAPACITY 2048 // Must be a power of 2

// Forward declarations
typedef struct fiber_s fiber_t;
typedef fiber_t* task_t;
typedef struct { WC_AtomicSize top; WC_AtomicSize bottom; task_t* tasks; } lock_free_deque_t;

// Deque implementation is unchanged and thus omitted for brevity. It's included in the final code block.
static void deque_init(lock_free_deque_t* deque);
static void deque_destroy(lock_free_deque_t* deque);
static bool deque_push(lock_free_deque_t* deque, task_t task);
static bool deque_pop(lock_free_deque_t* deque, task_t* task);
static bool deque_steal(lock_free_deque_t* deque, task_t* task);
