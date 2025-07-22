#include "task.h"
#include "thread_pool.h"
#include "atomic.h"
#include "memory.h"
#include "debug.h"

#include <string.h>

//-------------------------------------------------------------------------------------------------
// Internal struct definitions - hidden from header
//-------------------------------------------------------------------------------------------------

struct WC_Task {
    // Function and data
    WC_TaskFunction function;
    void* data;

    // Task metadata
    WC_AtomicU64 state;          // WC_TaskState
    uint32_t priority;           // WC_TaskPriority
    uint32_t affinity_mask;      // Preferred worker threads (bitmask)
    uint32_t estimated_cycles;   // Estimated execution time for load balancing

    // Dependency management
    WC_AtomicU64 incoming_deps;  // Number of dependencies that must complete first
    WC_Task** outgoing_deps;     // Tasks that depend on this one
    uint32_t outgoing_count;
    uint32_t outgoing_capacity;

    // Hierarchical tasks
    WC_TaskGroup* group;         // Group this task belongs to (optional)
    WC_Task* parent;             // Parent task (for hierarchical decomposition)

    // Memory management
    void* arena;                 // Arena to reset when task completes (opaque pointer)

    // Performance tracking
    uint64_t created_time;
    uint64_t started_time;
    uint64_t completed_time;
    uint32_t worker_id;          // Which worker executed this task
};

struct WC_TaskGroup {
    WC_AtomicU64 remaining_tasks; // How many tasks in this group are still pending
    WC_Task* continuation;        // Task to execute when group completes
    void* group_arena;            // Shared memory for all tasks in group (opaque pointer)

    // Group metadata
    uint32_t total_tasks;
    uint64_t created_time;
    bool auto_destroy;           // Destroy group when all tasks complete
};

// Cooperative task wrapper data
typedef struct {
    WC_CooperativeTaskFunction coop_func;
    void* user_data;
} WC_CoopWrapper;

//-------------------------------------------------------------------------------------------------
// Task memory management
//-------------------------------------------------------------------------------------------------

static void* g_task_pool = NULL;        // WC_PoolAllocator* (opaque)
static void* g_task_group_pool = NULL;  // WC_PoolAllocator* (opaque)

static uint64_t wc_get_time_ns(void) {
    // Simple cross-platform timer - replace with your preferred timing
    static uint64_t start_time = 0;
    if (start_time == 0) {
        start_time = 1; // Avoid zero
    }
    return ++start_time * 1000000; // Fake nanosecond counter
}

static void wc_ensure_task_pools(void) {
    if (!g_task_pool) {
        g_task_pool = wc_create_pool_allocator(sizeof(WC_Task), 1024);
    }
    if (!g_task_group_pool) {
        g_task_group_pool = wc_create_pool_allocator(sizeof(WC_TaskGroup), 128);
    }
}

//-------------------------------------------------------------------------------------------------
// Task creation and management
//-------------------------------------------------------------------------------------------------

WC_Task* wc_task_create(WC_TaskFunction function, void* data) {
    return wc_task_create_advanced(function, data, WC_TASK_PRIORITY_NORMAL, 0);
}

WC_Task* wc_task_create_advanced(WC_TaskFunction function, void* data,
                                 WC_TaskPriority priority, uint32_t affinity_mask) {
    WC_ASSERT(function);

    wc_ensure_task_pools();

	WC_Task* task = wc_pool_alloc(g_task_pool);
    if (!task) {
        return NULL;
    }

    memset(task, 0, sizeof(WC_Task));

    // Initialize basic task data
    task->function = function;
    task->data = data;
    task->priority = priority;
    task->affinity_mask = affinity_mask;
    task->created_time = wc_get_time_ns();

    // Initialize atomic fields
    task->state = wc_atomic_u64_create(WC_TASK_PENDING);
    task->incoming_deps = wc_atomic_u64_create(0);

    if (!task->state || !task->incoming_deps) {
        if (task->state) wc_atomic_u64_destroy(task->state);
        if (task->incoming_deps) wc_atomic_u64_destroy(task->incoming_deps);
        wc_pool_free(g_task_pool, task);
        return NULL;
    }

    // Allocate dependency arrays with initial capacity
    task->outgoing_capacity = 4;
    task->outgoing_deps = wc_malloc(task->outgoing_capacity * sizeof(WC_Task*));
    if (!task->outgoing_deps) {
        wc_atomic_u64_destroy(task->state);
        wc_atomic_u64_destroy(task->incoming_deps);
        wc_pool_free(g_task_pool, task);
        return NULL;
    }

    return task;
}

void wc_task_destroy(WC_Task* task) {
    if (!task) return;

	WC_Task* impl = task;

    WC_ASSERT(wc_atomic_u64_load(impl->state) == WC_TASK_COMPLETED ||
              wc_atomic_u64_load(impl->state) == WC_TASK_CANCELLED);

    // Free dependency arrays
    if (impl->outgoing_deps) {
        wc_free(impl->outgoing_deps);
    }

    // Destroy atomic fields
    wc_atomic_u64_destroy(impl->state);
    wc_atomic_u64_destroy(impl->incoming_deps);

    // Reset arena if it was assigned
    if (impl->arena) {
        // wc_arena_reset((WC_Arena*)impl->arena); // Would need to include arena header
    }

    wc_pool_free(g_task_pool, impl);
}

int wc_task_add_dependency(WC_Task* dependent_task, WC_Task* dependency_task) {
    WC_ASSERT(dependent_task && dependency_task);

	WC_Task* dependent = dependent_task;
	WC_Task* dependency = dependency_task;

    WC_ASSERT(wc_atomic_u64_load(dependent->state) == WC_TASK_PENDING);
    WC_ASSERT(wc_atomic_u64_load(dependency->state) != WC_TASK_COMPLETED);

    // Add to dependency's outgoing list
    if (dependency->outgoing_count >= dependency->outgoing_capacity) {
        // Resize outgoing dependency array
        uint32_t new_capacity = dependency->outgoing_capacity * 2;
        WC_Task** new_array = wc_realloc(dependency->outgoing_deps,
                                         new_capacity * sizeof(WC_Task*));
        if (!new_array) {
            return -1;
        }
        dependency->outgoing_deps = new_array;
        dependency->outgoing_capacity = new_capacity;
    }

    dependency->outgoing_deps[dependency->outgoing_count++] = dependent_task;

    // Increment dependent's incoming count
    wc_atomic_u64_fetch_add(dependent->incoming_deps, 1);

    return 0;
}

int wc_task_submit(WC_Task* task) {
    WC_ASSERT(task);

    WC_WorkStealingPool* pool = wc_get_global_pool();
    if (!pool) {
        return -1;
    }

	WC_Task* impl = task;

    // Check if task is ready to run (no dependencies)
    if (wc_atomic_u64_load(impl->incoming_deps) == 0) {
        wc_atomic_u64_store(impl->state, WC_TASK_READY);
        return wc_pool_submit_task(pool, task);
    } else {
        // Task will be submitted when dependencies complete
        return 0;
    }
}

int wc_task_submit_batch(WC_Task** tasks, uint32_t count) {
    WC_ASSERT(tasks && count > 0);

    WC_WorkStealingPool* pool = wc_get_global_pool();
    if (!pool) {
        return -1;
    }

    // Separate ready tasks from pending tasks
    WC_Task** ready_tasks = wc_malloc(count * sizeof(WC_Task*));
    if (!ready_tasks) {
        return -1;
    }

    uint32_t ready_count = 0;

    for (uint32_t i = 0; i < count; i++) {
		WC_Task* impl = tasks[i];
        if (wc_atomic_u64_load(impl->incoming_deps) == 0) {
            wc_atomic_u64_store(impl->state, WC_TASK_READY);
            ready_tasks[ready_count++] = tasks[i];
        }
    }

    // Submit ready tasks to pool
    int result = (ready_count > 0) ? wc_pool_submit_batch(pool, ready_tasks, ready_count) : 0;

    wc_free(ready_tasks);
    return result;
}

void wc_task_wait(WC_Task* task) {
    WC_ASSERT(task);

	WC_Task* impl = task;
    WC_WorkStealingPool* pool = wc_get_global_pool();

    // Keep processing tasks until this one completes
    while (wc_atomic_u64_load(impl->state) != WC_TASK_COMPLETED &&
           wc_atomic_u64_load(impl->state) != WC_TASK_CANCELLED) {

        if (pool) {
            wc_pool_process_tasks(pool, 1);
        }

        // Small pause to avoid busy waiting
        wc_cpu_pause();
    }
}

void wc_task_wait_all(WC_Task** tasks, uint32_t count) {
    WC_ASSERT(tasks && count > 0);

    WC_WorkStealingPool* pool = wc_get_global_pool();

    bool all_complete = false;
    while (!all_complete) {
        all_complete = true;

        for (uint32_t i = 0; i < count; i++) {
			WC_Task* impl = tasks[i];
            WC_TaskState state = (WC_TaskState)wc_atomic_u64_load(impl->state);
            if (state != WC_TASK_COMPLETED && state != WC_TASK_CANCELLED) {
                all_complete = false;
                break;
            }
        }

        if (!all_complete && pool) {
            wc_pool_process_tasks(pool, 1);
        }
    }
}

//-------------------------------------------------------------------------------------------------
// Task groups
//-------------------------------------------------------------------------------------------------

WC_TaskGroup* wc_task_group_create(uint32_t estimated_task_count) {
    wc_ensure_task_pools();

	WC_TaskGroup* group = wc_pool_alloc(g_task_group_pool);
    if (!group) {
        return NULL;
    }

    memset(group, 0, sizeof(WC_TaskGroup));

    // Initialize atomic fields
    group->remaining_tasks = wc_atomic_u64_create(0);
    if (!group->remaining_tasks) {
        wc_pool_free(g_task_group_pool, group);
        return NULL;
    }

    group->total_tasks = 0;
    group->created_time = wc_get_time_ns();
    group->auto_destroy = true;

    // Create group arena (would need arena implementation)
    // group->group_arena = wc_malloc(sizeof(WC_Arena));
    // ...

    return group;
}

void wc_task_group_destroy(WC_TaskGroup* group) {
    if (!group) return;

	WC_TaskGroup* impl = group;

    WC_ASSERT(wc_atomic_u64_load(impl->remaining_tasks) == 0);

    wc_atomic_u64_destroy(impl->remaining_tasks);

    if (impl->group_arena) {
        // wc_arena_free((WC_Arena*)impl->group_arena);
        // wc_free(impl->group_arena);
    }

    wc_pool_free(g_task_group_pool, impl);
}

int wc_task_group_add(WC_TaskGroup* group, WC_Task* task) {
    WC_ASSERT(group && task);

	WC_TaskGroup* group_impl = group;
	WC_Task* task_impl = task;

    WC_ASSERT(wc_atomic_u64_load(task_impl->state) == WC_TASK_PENDING);

    task_impl->group = group;
    task_impl->arena = group_impl->group_arena;

    wc_atomic_u64_fetch_add(group_impl->remaining_tasks, 1);
    group_impl->total_tasks++;

    return 0;
}

WC_Arena* wc_task_group_get_arena(WC_TaskGroup* group)
{
	WC_ASSERT(group);
	return group->group_arena;
}

//-------------------------------------------------------------------------------------------------
// Task utilities and getters
//-------------------------------------------------------------------------------------------------

WC_TaskState wc_task_get_state(const WC_Task* task) {
    WC_ASSERT(task);
	WC_Task* impl = (struct WC_Task*)task;
    return (WC_TaskState)wc_atomic_u64_load(impl->state);
}

uint32_t wc_task_get_priority(const WC_Task* task) {
    WC_ASSERT(task);
	WC_Task* impl = (struct WC_Task*)task;
    return impl->priority;
}

void wc_task_set_priority(WC_Task* task, uint32_t priority) {
    WC_ASSERT(task);
	WC_Task* impl = task;
    impl->priority = priority;
}

WC_TaskPerfInfo wc_task_get_perf_info(const WC_Task* task) {
    WC_ASSERT(task);

	WC_Task* impl = (struct WC_Task*)task;

    WC_TaskPerfInfo info = {0};
    info.creation_time = impl->created_time;
    info.start_time = impl->started_time;
    info.completion_time = impl->completed_time;
    info.worker_id = impl->worker_id;
    info.dependency_count = impl->outgoing_count;

    if (impl->completed_time > impl->started_time) {
        info.execution_duration = impl->completed_time - impl->started_time;
    }

    if (impl->started_time > impl->created_time) {
        info.wait_duration = impl->started_time - impl->created_time;
    }

    return info;
}

//-------------------------------------------------------------------------------------------------
// Cooperative tasks
//-------------------------------------------------------------------------------------------------

static void wc_cooperative_task_wrapper(void* data) {
    WC_CoopWrapper* wrapper = data;
    WC_TaskYield result = WC_TASK_CONTINUE;

    do {
        result = wrapper->coop_func(wrapper->user_data);

        if (result == WC_TASK_YIELD) {
            // Reschedule this task
            WC_Task* current_task = wc_task_get_current();
            if (current_task) {
                wc_task_submit(current_task);
                wc_free(wrapper); // Cleanup wrapper
                return; // Exit current execution
            }
        }
    } while (result == WC_TASK_CONTINUE);

    // Task completed, cleanup wrapper
    wc_free(wrapper);
}

WC_Task* wc_task_create_cooperative(WC_CooperativeTaskFunction function, void* data) {
	WC_CoopWrapper* wrapper = wc_malloc(sizeof(WC_CoopWrapper));
	if (!wrapper) {
		return NULL;
	}

	wrapper->coop_func = function;
	wrapper->user_data = data;

	return wc_task_create(wc_cooperative_task_wrapper, wrapper);
}

//-------------------------------------------------------------------------------------------------
// Task completion handling (internal)
//-------------------------------------------------------------------------------------------------

void wc_task_complete_internal(WC_Task* task) {
    WC_ASSERT(task);

	WC_Task* impl = task;

    // Notify all dependent tasks
    for (uint32_t i = 0; i < impl->outgoing_count; i++) {
		WC_Task* dependent = impl->outgoing_deps[i];

        uint64_t remaining = wc_atomic_u64_fetch_sub(dependent->incoming_deps, 1) - 1;

        if (remaining == 0) {
            // This dependent is now ready to run
            wc_atomic_u64_store(dependent->state, WC_TASK_READY);

            WC_WorkStealingPool* pool = wc_get_global_pool();
            if (pool) {
                wc_pool_submit_task(pool, impl->outgoing_deps[i]);
            }
        }
    }

    // Handle task group completion
    if (impl->group) {
		WC_TaskGroup* group = impl->group;
        uint64_t remaining = wc_atomic_u64_fetch_sub(group->remaining_tasks, 1) - 1;

        if (remaining == 0) {
            // Group is complete
            if (group->continuation) {
                wc_task_submit(group->continuation);
            }

            if (group->auto_destroy) {
                wc_task_group_destroy(impl->group);
            }
        }
    }
}

//-------------------------------------------------------------------------------------------------
// Utility functions
//-------------------------------------------------------------------------------------------------

WC_Task* wc_task_get_current(void) {
    WC_WorkerThread* worker = wc_pool_get_current_worker();
    return worker ? wc_worker_get_current_task(worker) : NULL;
}

bool wc_task_is_executing(void) {
    return wc_task_get_current() != NULL;
}

uint32_t wc_task_get_worker_id(void) {
    WC_WorkerThread* worker = wc_pool_get_current_worker();
    return worker ? wc_worker_get_id(worker) : 0;
}

void wc_task_yield(void) {
    // This would need to be implemented based on cooperative task system
    // For now, just a no-op
}

// Stubs for other functions that depend on missing implementations
WC_TaskStats wc_task_get_stats(void) {
    WC_TaskStats stats = {0};
    return stats;
}

void wc_task_reset_stats(void) {
    // No-op for now
}

//-------------------------------------------------------------------------------------------------
// Task accessors for thread pool
//-------------------------------------------------------------------------------------------------

WC_TaskFunction wc_task_get_function(const WC_Task* task) {
	WC_ASSERT(task);
	const struct WC_Task* impl = task;
	return impl->function;
}

void* wc_task_get_data(const WC_Task* task) {
	WC_ASSERT(task);
	const struct WC_Task* impl = task;
	return impl->data;
}

void wc_task_set_started_time(WC_Task* task, uint64_t time) {
	WC_ASSERT(task);
	struct WC_Task* impl = task;
	impl->started_time = time;
}

void wc_task_set_completed_time(WC_Task* task, uint64_t time) {
	WC_ASSERT(task);
	struct WC_Task* impl = task;
	impl->completed_time = time;
}

void wc_task_set_worker_id(WC_Task* task, uint32_t worker_id) {
	WC_ASSERT(task);
	struct WC_Task* impl = task;
	impl->worker_id = worker_id;
}

uint64_t wc_task_get_started_time(const WC_Task* task) {
	WC_ASSERT(task);
	const struct WC_Task* impl = task;
	return impl->started_time;
}

uint64_t wc_task_get_completed_time(const WC_Task* task) {
	WC_ASSERT(task);
	const struct WC_Task* impl = task;
	return impl->completed_time;
}

void wc_task_set_state(WC_Task* task, WC_TaskState state) {
	WC_ASSERT(task);
	struct WC_Task* impl = task;
	wc_atomic_u64_store(impl->state, (uint64_t)state);
}

void wc_task_set_arena(WC_Task* task, WC_Arena* arena) {
	WC_ASSERT(task);
	struct WC_Task* impl = task;
	impl->arena = arena;
}

WC_Arena* wc_task_get_arena(const WC_Task* task) {
	WC_ASSERT(task);
	const struct WC_Task* impl = task;
	return impl->arena;
}