#pragma once

//-------------------------------------------------------------------------------------------------
// Lock-free work-stealing deque - lean header
//-------------------------------------------------------------------------------------------------

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Forward declarations - no includes needed
typedef struct WC_Task WC_Task;
typedef struct WC_Deque WC_Deque;
typedef struct WC_CircularArray WC_CircularArray;

typedef enum WC_DequeResult {
    WC_DEQUE_SUCCESS,
    WC_DEQUE_EMPTY,
    WC_DEQUE_ABORTED,    // CAS failed during steal
    WC_DEQUE_RESIZE_NEEDED
} WC_DequeResult;

typedef struct WC_DequeStats {
    uint64_t total_pushes;
    uint64_t total_pops;
    uint64_t total_steals_attempted;
    uint64_t total_steals_succeeded;
    double steal_success_rate;
    size_t current_capacity;
    size_t current_size;
} WC_DequeStats;

//-------------------------------------------------------------------------------------------------
// Deque operations - implementation in deque.c
//-------------------------------------------------------------------------------------------------

// Create/destroy deque
WC_Deque* wc_deque_create(size_t initial_capacity);
void wc_deque_destroy(WC_Deque* deque);

// Core operations
WC_DequeResult wc_deque_push_bottom(WC_Deque* deque, WC_Task* task);
WC_Task* wc_deque_pop_bottom(WC_Deque* deque);
WC_Task* wc_deque_steal_top(WC_Deque* deque);

// Utilities
size_t wc_deque_size(const WC_Deque* deque);
bool wc_deque_is_empty(const WC_Deque* deque);
WC_DequeResult wc_deque_resize(WC_Deque* deque);

// Statistics
WC_DequeStats wc_deque_get_stats(const WC_Deque* deque);
void wc_deque_reset_stats(WC_Deque* deque);

//-------------------------------------------------------------------------------------------------
// Internal helpers (for implementation)
//-------------------------------------------------------------------------------------------------

WC_CircularArray* wc_circular_array_create(size_t capacity);
void wc_circular_array_destroy(WC_CircularArray* array);