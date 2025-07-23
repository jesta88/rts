#include "deque.h"

#include "SDL3/SDL_log.h"
#include "atomic.h"
#include "core.h"
#include "debug.h"
#include "memory.h"

//-------------------------------------------------------------------------------------------------
// Internal struct definitions - hidden from header
//-------------------------------------------------------------------------------------------------

struct WC_CircularArray {
    size_t capacity;                    // Must be power of 2
    _Alignas(64) WC_AtomicPtr* elements; // Array of atomic pointers
};

struct WC_Deque {
    _Alignas(64) WC_AtomicSize top;     // Thieves steal from here
    _Alignas(64) WC_AtomicSize bottom;  // Owner pushes/pops here
    _Alignas(64) WC_AtomicPtr buffer;   // Points to WC_CircularArray*

    // Statistics (on separate cache line to avoid false sharing)
    _Alignas(64) uint64_t total_pushes;
    uint64_t total_pops;
    uint64_t total_steals_attempted;
    uint64_t total_steals_succeeded;
};

//-------------------------------------------------------------------------------------------------
// Constants and helpers
//-------------------------------------------------------------------------------------------------

#define WC_DEQUE_MIN_CAPACITY 64
#define WC_DEQUE_MAX_CAPACITY (1ULL << 48)  // Large but reasonable limit for x64

// Ensure capacity is power of 2
static size_t wc_next_power_of_2(size_t n) {
    if (n <= 1) return 1;

    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    n++;

    return n;
}

//-------------------------------------------------------------------------------------------------
// Circular array management
//-------------------------------------------------------------------------------------------------

WC_CircularArray* wc_circular_array_create(size_t capacity) {
    WC_ASSERT(capacity >= WC_DEQUE_MIN_CAPACITY);
    WC_ASSERT((capacity & (capacity - 1)) == 0); // Must be power of 2

    WC_CircularArray* array = wc_aligned_alloc(sizeof(WC_CircularArray), 64);
    if (!array) {
        return NULL;
    }

    array->capacity = capacity;
    array->elements = wc_aligned_alloc(capacity * sizeof(WC_AtomicPtr), 64);
    if (!array->elements) {
        wc_aligned_free(array, 64);
        return NULL;
    }

    // Initialize all elements to NULL
    for (size_t i = 0; i < capacity; i++) {
        array->elements[i] = wc_atomic_ptr_create(NULL);
        if (!array->elements[i]) {
            // Cleanup on failure
            for (size_t j = 0; j < i; j++) {
                wc_atomic_ptr_destroy(array->elements[j]);
            }
            wc_aligned_free(array->elements, 64);
            wc_aligned_free(array, 64);
            return NULL;
        }
    }

    return array;
}

void wc_circular_array_destroy(WC_CircularArray* array) {
    if (array) {
        if (array->elements) {
            for (size_t i = 0; i < array->capacity; i++) {
                wc_atomic_ptr_destroy(array->elements[i]);
            }
            wc_aligned_free(array->elements, 64);
        }
        wc_aligned_free(array, 64);
    }
}

//-------------------------------------------------------------------------------------------------
// Deque operations
//-------------------------------------------------------------------------------------------------

WC_Deque* wc_deque_create(size_t initial_capacity) {
    if (initial_capacity < WC_DEQUE_MIN_CAPACITY) {
        initial_capacity = WC_DEQUE_MIN_CAPACITY;
    }

    initial_capacity = wc_next_power_of_2(initial_capacity);

    WC_CircularArray* array = wc_circular_array_create(initial_capacity);
    if (!array) {
        return NULL;
    }

    WC_Deque* deque = wc_aligned_alloc(sizeof(WC_Deque), 64);
    if (!deque) {
        wc_circular_array_destroy(array);
        return NULL;
    }

    deque->top = wc_atomic_size_create(0);
    deque->bottom = wc_atomic_size_create(0);
    deque->buffer = wc_atomic_ptr_create(array);

    if (!deque->top || !deque->bottom || !deque->buffer) {
        if (deque->top) wc_atomic_size_destroy(deque->top);
        if (deque->bottom) wc_atomic_size_destroy(deque->bottom);
        if (deque->buffer) wc_atomic_ptr_destroy(deque->buffer);
        wc_aligned_free(deque, 64);
        wc_circular_array_destroy(array);
        return NULL;
    }

    // Initialize statistics
    deque->total_pushes = 0;
    deque->total_pops = 0;
    deque->total_steals_attempted = 0;
    deque->total_steals_succeeded = 0;

    return deque;
}

void wc_deque_destroy(WC_Deque* deque) {
    if (!deque) return;

    WC_CircularArray* array = (WC_CircularArray*)wc_atomic_ptr_load(deque->buffer);
    wc_circular_array_destroy(array);

    wc_atomic_size_destroy(deque->top);
    wc_atomic_size_destroy(deque->bottom);
    wc_atomic_ptr_destroy(deque->buffer);

    wc_aligned_free(deque, 64);
}

WC_DequeResult wc_deque_push_bottom(WC_Deque* deque, WC_Task* task) {
    WC_ASSERT(deque && task);

    size_t bottom = wc_atomic_size_load(deque->bottom);
    WC_CircularArray* array = (WC_CircularArray*)wc_atomic_ptr_load(deque->buffer);

    // Store the task in the array
    wc_atomic_ptr_store(array->elements[bottom & (array->capacity - 1)], task);

    // Critical: Memory fence to ensure task is written before bottom update
    wc_atomic_fence_release();

    // Update bottom pointer
    wc_atomic_size_store(deque->bottom, bottom + 1);

    // Update statistics
    deque->total_pushes++;

    // Check if we need to resize
    size_t top = wc_atomic_size_load_acquire(deque->top);
    if (bottom + 1 - top >= array->capacity) {
        return WC_DEQUE_RESIZE_NEEDED;
    }

    return WC_DEQUE_SUCCESS;
}

WC_Task* wc_deque_pop_bottom(WC_Deque* deque) {
    WC_ASSERT(deque);

    WC_CircularArray* array = (WC_CircularArray*)wc_atomic_ptr_load(deque->buffer);

    size_t bottom = wc_atomic_size_load(deque->bottom);
    bottom = bottom - 1;

    // Tentatively update bottom
    wc_atomic_size_store(deque->bottom, bottom);

    // Memory fence - critical for correctness!
    wc_atomic_fence_seq_cst();

    size_t top = wc_atomic_size_load(deque->top);

    WC_Task* task = NULL;

    if (top <= bottom) {
        // Non-empty queue
        task = (WC_Task*)wc_atomic_ptr_load(array->elements[bottom & (array->capacity - 1)]);

        if (top == bottom) {
            // Last element - potential race with steal!
            if (!wc_atomic_size_cas(deque->top, top, top + 1)) {
                // CAS failed - thief got the last element
                task = NULL;
            }
            wc_atomic_size_store(deque->bottom, bottom + 1);
        }
    } else {
        // Empty queue
        wc_atomic_size_store(deque->bottom, bottom + 1);
    }

    // Update statistics
    if (task) {
        deque->total_pops++;
    }

    return task;
}

WC_Task* wc_deque_steal_top(WC_Deque* deque) {
    WC_ASSERT(deque);

	if (!deque) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Attempting to steal from null deque!");
		return NULL;
	}

    size_t top = wc_atomic_size_load_acquire(deque->top);

    // Memory fence to ensure we see all writes before this top value
    wc_atomic_fence_seq_cst();

    size_t bottom = wc_atomic_size_load_acquire(deque->bottom);

    deque->total_steals_attempted++;

    if (top < bottom) {
        // Queue appears non-empty
        WC_CircularArray* array = (WC_CircularArray*)wc_atomic_ptr_load_acquire(deque->buffer);

        WC_Task* task = (WC_Task*)wc_atomic_ptr_load(array->elements[top & (array->capacity - 1)]);

        // Try to claim this element with CAS
        if (wc_atomic_size_cas(deque->top, top, top + 1)) {
            // CAS succeeded - we got the task
            deque->total_steals_succeeded++;
            return task;
        }
    }

    return NULL; // Empty queue or CAS failed
}

WC_DequeResult wc_deque_resize(WC_Deque* deque) {
    WC_ASSERT(deque);

    WC_CircularArray* old_array = (WC_CircularArray*)wc_atomic_ptr_load(deque->buffer);

    size_t old_capacity = old_array->capacity;
    size_t new_capacity = old_capacity * 2;

    if (new_capacity > WC_DEQUE_MAX_CAPACITY) {
        return WC_DEQUE_ABORTED; // Cannot resize further
    }

    // Allocate new array
    WC_CircularArray* new_array = wc_circular_array_create(new_capacity);
    if (!new_array) {
        return WC_DEQUE_ABORTED;
    }

    // Copy elements (only owner does this, so no races on the elements themselves)
    size_t top = wc_atomic_size_load(deque->top);
    size_t bottom = wc_atomic_size_load(deque->bottom);

    for (size_t i = top; i < bottom; i++) {
        void* element = wc_atomic_ptr_load(old_array->elements[i & (old_capacity - 1)]);
        wc_atomic_ptr_store(new_array->elements[i & (new_capacity - 1)], element);
    }

    // Atomic pointer swap - this is the critical moment!
    wc_atomic_ptr_store_release(deque->buffer, new_array);

    // The old array cannot be freed immediately because other threads might still be
    // accessing it. In a production system, you'd use RCU or hazard pointers.
    // For now, we'll leak it (acceptable for demonstration purposes).
    // TODO: Implement proper memory reclamation

    return WC_DEQUE_SUCCESS;
}

size_t wc_deque_size(const WC_Deque* deque) {
    WC_ASSERT(deque);

    size_t bottom = wc_atomic_size_load(deque->bottom);
    size_t top = wc_atomic_size_load(deque->top);

    // Handle potential wraparound
    if (bottom >= top) {
        return bottom - top;
    } else {
        return 0;
    }
}

bool wc_deque_is_empty(const WC_Deque* deque) {
    return wc_deque_size(deque) == 0;
}

//-------------------------------------------------------------------------------------------------
// Statistics
//-------------------------------------------------------------------------------------------------

WC_DequeStats wc_deque_get_stats(const WC_Deque* deque) {
    WC_ASSERT(deque);

    WC_DequeStats stats = {0};
    stats.total_pushes = deque->total_pushes;
    stats.total_pops = deque->total_pops;
    stats.total_steals_attempted = deque->total_steals_attempted;
    stats.total_steals_succeeded = deque->total_steals_succeeded;

    if (stats.total_steals_attempted > 0) {
        stats.steal_success_rate = (double)stats.total_steals_succeeded /
                                  (double)stats.total_steals_attempted;
    }

    WC_CircularArray* array = (WC_CircularArray*)wc_atomic_ptr_load(deque->buffer);
    stats.current_capacity = array->capacity;
    stats.current_size = wc_deque_size(deque);

    return stats;
}

void wc_deque_reset_stats(WC_Deque* deque) {
    WC_ASSERT(deque);

    deque->total_pushes = 0;
    deque->total_pops = 0;
    deque->total_steals_attempted = 0;
    deque->total_steals_succeeded = 0;
}