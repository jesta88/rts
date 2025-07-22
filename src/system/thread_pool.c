#include "thread_pool.h"

#include "atomic.h"
#include "debug.h"
#include "deque.h"
#include "memory.h"
#include "task.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <stdlib.h>
#include <string.h>

//-------------------------------------------------------------------------------------------------
// Internal structures
//-------------------------------------------------------------------------------------------------

struct WC_WorkerThread {
    uint32_t thread_id;
    WC_WorkStealingPool* pool;
    SDL_Thread* handle;

    // Work queue
    WC_Deque* local_queue;

    // Thread state
    WC_AtomicBool active;
    WC_Task* current_task;
    uint64_t task_start_time;

    // Thread-local arena
    WC_Arena* thread_arena;

    // Statistics
    uint64_t tasks_executed;
    uint64_t local_pushes;
    uint64_t local_pops;
    uint64_t steals_attempted;
    uint64_t steals_succeeded;
    uint64_t idle_time;

    // Random state for victim selection
    uint32_t random_state;
};

struct WC_WorkStealingPool {
    // Workers
    WC_WorkerThread* workers;
    uint32_t worker_count;

    // Global queues
    WC_Deque* global_queue;
    WC_Deque* high_priority_queue;

    // Pool state
    WC_AtomicBool shutdown;
    WC_AtomicU64 active_workers;
    WC_AtomicU64 sleeping_workers;

    // Synchronization
    SDL_Mutex* sleep_mutex;
    SDL_Condition* wake_condition;

    // Statistics
    WC_AtomicU64 total_tasks_submitted;
    WC_AtomicU64 total_tasks_completed;
    WC_AtomicU64 total_steal_attempts;
    WC_AtomicU64 total_steal_successes;

    // Configuration
    uint32_t max_idle_spins;
    uint32_t steal_attempts_per_round;
    bool enable_work_stealing;
    bool enable_numa_awareness;
};

//-------------------------------------------------------------------------------------------------
// Thread-local storage
//-------------------------------------------------------------------------------------------------

static SDL_TLSID tls_current_worker;
static bool tls_initialized = false;

//-------------------------------------------------------------------------------------------------
// Global pool instance
//-------------------------------------------------------------------------------------------------

static WC_WorkStealingPool* g_global_pool = NULL;
static bool g_global_pool_initialized = false;

//-------------------------------------------------------------------------------------------------
// Utility functions
//-------------------------------------------------------------------------------------------------

static uint64_t wc_get_time_ns(void) {
    return SDL_GetTicksNS();
}

static uint32_t wc_get_cpu_count(void) {
    return SDL_GetNumLogicalCPUCores();
}

// Fast pseudo-random number generator (xorshift32)
static uint32_t wc_random_next(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// Pin thread to specific CPU core (SDL3 doesn't provide this directly)
static void wc_pin_thread_to_core(uint32_t core_id) {
    // SDL3 doesn't have direct CPU affinity support
    // This could be implemented per-platform if needed
    // For now, we'll skip thread pinning
    (void)core_id; // Suppress unused parameter warning
}

//-------------------------------------------------------------------------------------------------
// Forward declarations
//-------------------------------------------------------------------------------------------------

static void wc_pool_worker_sleep(WC_WorkerThread* worker);
static void wc_pool_numa_init(WC_WorkStealingPool* pool);
static uint32_t wc_pool_select_numa_victim(WC_WorkerThread* thief);

//-------------------------------------------------------------------------------------------------
// Worker thread main loop
//-------------------------------------------------------------------------------------------------

static int worker_thread_main(void* arg) {
    WC_WorkerThread* worker = (WC_WorkerThread*)arg;
    WC_WorkStealingPool* pool = worker->pool;

    // Set thread-local storage
    SDL_SetTLS(&tls_current_worker, worker, NULL);

    // Pin to CPU core for better cache locality
    if (pool->enable_numa_awareness && worker->thread_id > 0) {
        wc_pin_thread_to_core(worker->thread_id - 1); // -1 because thread_id 0 is main thread
    }

    // Initialize random state with unique seed
    worker->random_state = (uint32_t)(worker->thread_id * 0x9e3779b9u + (uint32_t)wc_get_time_ns());

    wc_atomic_bool_store(worker->active, true);
    wc_atomic_u64_fetch_add(pool->active_workers, 1);

    uint32_t idle_spins = 0;

    while (!wc_atomic_bool_load(pool->shutdown)) {
        WC_Task* task = NULL;

        // Phase 1: Try to get work from local queue
        task = wc_deque_pop_bottom(worker->local_queue);
        if (task) {
            worker->local_pops++;
            idle_spins = 0;
        } else {
            // Phase 2: Try to steal work from other workers
            task = wc_pool_steal_work(worker);
            if (task) {
                idle_spins = 0;
            } else {
                // Phase 3: Check global queues
                task = wc_deque_steal_top(pool->high_priority_queue);
                if (!task) {
                    task = wc_deque_steal_top(pool->global_queue);
                }

                if (task) {
                    idle_spins = 0;
                } else {
                    // No work found - idle handling
                    idle_spins++;

                    if (idle_spins < pool->max_idle_spins) {
                        // Busy wait with CPU pause
                        wc_cpu_pause();
                    } else {
                        // Sleep until woken up
                        wc_pool_worker_sleep(worker);
                        idle_spins = 0;
                    }
                }
            }
        }

        // Execute the task if we found one
        if (task) {
            wc_pool_execute_task(worker, task);
        }
    }

    wc_atomic_bool_store(worker->active, false);
    wc_atomic_u64_fetch_sub(pool->active_workers, 1);

    return 0;
}

//-------------------------------------------------------------------------------------------------
// Pool management
//-------------------------------------------------------------------------------------------------

WC_WorkStealingPool* wc_pool_create(uint32_t worker_count) {
    WC_ASSERT(worker_count > 0 && worker_count <= 64);

    WC_WorkStealingPool* pool = wc_aligned_alloc(sizeof(WC_WorkStealingPool), 64);
    if (!pool) {
        return NULL;
    }

    memset(pool, 0, sizeof(WC_WorkStealingPool));

    // Default configuration
    pool->max_idle_spins = 1000;
    pool->steal_attempts_per_round = 4;
    pool->enable_work_stealing = true;
    pool->enable_numa_awareness = true;

    // Initialize atomic fields
    pool->shutdown = wc_atomic_bool_create(false);
    pool->active_workers = wc_atomic_u64_create(0);
    pool->sleeping_workers = wc_atomic_u64_create(0);
    pool->total_tasks_submitted = wc_atomic_u64_create(0);
    pool->total_tasks_completed = wc_atomic_u64_create(0);
    pool->total_steal_attempts = wc_atomic_u64_create(0);
    pool->total_steal_successes = wc_atomic_u64_create(0);

    if (!pool->shutdown || !pool->active_workers || !pool->sleeping_workers ||
        !pool->total_tasks_submitted || !pool->total_tasks_completed ||
        !pool->total_steal_attempts || !pool->total_steal_successes) {
        wc_pool_destroy(pool);
        return NULL;
    }

    // Initialize synchronization primitives
    pool->sleep_mutex = SDL_CreateMutex();
    if (!pool->sleep_mutex) {
        wc_pool_destroy(pool);
        return NULL;
    }

    pool->wake_condition = SDL_CreateCondition();
    if (!pool->wake_condition) {
        wc_pool_destroy(pool);
        return NULL;
    }

    // Initialize global queues
    pool->global_queue = wc_deque_create(1024);
    pool->high_priority_queue = wc_deque_create(512);
    if (!pool->global_queue || !pool->high_priority_queue) {
        wc_pool_destroy(pool);
        return NULL;
    }

    // Allocate worker array (+1 for main thread at index 0)
    pool->worker_count = worker_count + 1;
    pool->workers = wc_aligned_alloc(pool->worker_count * sizeof(WC_WorkerThread), 64);
    if (!pool->workers) {
        wc_pool_destroy(pool);
        return NULL;
    }

    // Initialize thread-local storage for workers
    if (!tls_initialized) {
        tls_initialized = true;
    }

    // Initialize workers
    for (uint32_t i = 0; i < pool->worker_count; i++) {
        WC_WorkerThread* worker = &pool->workers[i];
        memset(worker, 0, sizeof(WC_WorkerThread));

        worker->thread_id = i;
        worker->pool = pool;
        worker->active = wc_atomic_bool_create(false);

        if (!worker->active) {
            wc_pool_destroy(pool);
            return NULL;
        }

        // Initialize local queue
        worker->local_queue = wc_deque_create(256);
        if (!worker->local_queue) {
            wc_pool_destroy(pool);
            return NULL;
        }

        // Create thread arena
        worker->thread_arena = wc_malloc(sizeof(WC_Arena));
        if (!worker->thread_arena || wc_arena_init(worker->thread_arena, 64 * 1024) != 0) {
            wc_pool_destroy(pool);
            return NULL;
        }

        // Create worker threads (skip index 0 which is the main thread)
        if (i > 0) {
            char thread_name[32];
            SDL_snprintf(thread_name, sizeof(thread_name), "WorkerThread-%u", i);

            worker->handle = SDL_CreateThread(worker_thread_main, thread_name, worker);
            if (!worker->handle) {
                wc_pool_destroy(pool);
                return NULL;
            }
        }
    }

    // Set main thread as worker 0
    SDL_SetTLS(&tls_current_worker, &pool->workers[0], NULL);

    // Initialize NUMA if enabled
    if (pool->enable_numa_awareness) {
        wc_pool_numa_init(pool);
    }

    return pool;
}

void wc_pool_destroy(WC_WorkStealingPool* pool) {
    if (!pool) return;

    // Signal shutdown
    if (pool->shutdown) {
        wc_atomic_bool_store(pool->shutdown, true);
    }

    // Wake up all sleeping workers
    wc_pool_wake_workers(pool, UINT32_MAX);

    // Wait for worker threads to finish
    if (pool->workers) {
        for (uint32_t i = 1; i < pool->worker_count; i++) { // Skip main thread
            WC_WorkerThread* worker = &pool->workers[i];
            if (worker->handle) {
                SDL_WaitThread(worker->handle, NULL);
                worker->handle = NULL;
            }

            // Cleanup worker resources
            if (worker->local_queue) {
                wc_deque_destroy(worker->local_queue);
            }
            if (worker->thread_arena) {
                wc_arena_free(worker->thread_arena);
                wc_free(worker->thread_arena);
            }
            if (worker->active) {
                wc_atomic_bool_destroy(worker->active);
            }
        }

        // Cleanup main thread worker
        if (pool->workers[0].local_queue) {
            wc_deque_destroy(pool->workers[0].local_queue);
        }
        if (pool->workers[0].thread_arena) {
            wc_arena_free(pool->workers[0].thread_arena);
            wc_free(pool->workers[0].thread_arena);
        }
        if (pool->workers[0].active) {
            wc_atomic_bool_destroy(pool->workers[0].active);
        }

        wc_aligned_free(pool->workers, 64);
    }

    // Cleanup global queues
    if (pool->global_queue) {
        wc_deque_destroy(pool->global_queue);
    }
    if (pool->high_priority_queue) {
        wc_deque_destroy(pool->high_priority_queue);
    }

    // Cleanup synchronization
    if (pool->wake_condition) {
        SDL_DestroyCondition(pool->wake_condition);
    }
    if (pool->sleep_mutex) {
        SDL_DestroyMutex(pool->sleep_mutex);
    }

    // Destroy atomic fields
    if (pool->shutdown) wc_atomic_bool_destroy(pool->shutdown);
    if (pool->active_workers) wc_atomic_u64_destroy(pool->active_workers);
    if (pool->sleeping_workers) wc_atomic_u64_destroy(pool->sleeping_workers);
    if (pool->total_tasks_submitted) wc_atomic_u64_destroy(pool->total_tasks_submitted);
    if (pool->total_tasks_completed) wc_atomic_u64_destroy(pool->total_tasks_completed);
    if (pool->total_steal_attempts) wc_atomic_u64_destroy(pool->total_steal_attempts);
    if (pool->total_steal_successes) wc_atomic_u64_destroy(pool->total_steal_successes);

    wc_aligned_free(pool, 64);
}

int wc_pool_submit_task(WC_WorkStealingPool* pool, WC_Task* task) {
    WC_ASSERT(pool && task);

    wc_atomic_u64_fetch_add(pool->total_tasks_submitted, 1);

    // Try to submit to current worker's local queue first
    WC_WorkerThread* current = wc_pool_get_current_worker();
    if (current) {
        WC_DequeResult result = wc_deque_push_bottom(current->local_queue, task);
        if (result == WC_DEQUE_SUCCESS) {
            current->local_pushes++;
            return 0;
        } else if (result == WC_DEQUE_RESIZE_NEEDED) {
            wc_deque_resize(current->local_queue);
            if (wc_deque_push_bottom(current->local_queue, task) == WC_DEQUE_SUCCESS) {
                current->local_pushes++;
                return 0;
            }
        }
    }

    // Local queue full or no current worker - use global queue
    uint32_t priority = wc_task_get_priority(task);
    WC_Deque* target_queue = (priority == WC_TASK_PRIORITY_HIGH ||
                             priority == WC_TASK_PRIORITY_CRITICAL)
                             ? pool->high_priority_queue
                             : pool->global_queue;

    WC_DequeResult result = wc_deque_push_bottom(target_queue, task);
    if (result == WC_DEQUE_RESIZE_NEEDED) {
        wc_deque_resize(target_queue);
        result = wc_deque_push_bottom(target_queue, task);
    }

    if (result == WC_DEQUE_SUCCESS) {
        // Wake up sleeping workers
        wc_pool_wake_workers(pool, 1);
        return 0;
    }

    return -1;
}

int wc_pool_submit_batch(WC_WorkStealingPool* pool, WC_Task** tasks, uint32_t count) {
    WC_ASSERT(pool && tasks && count > 0);

    uint32_t submitted = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (wc_pool_submit_task(pool, tasks[i]) == 0) {
            submitted++;
        }
    }

    return (submitted == count) ? 0 : -1;
}

void wc_pool_wait_idle(WC_WorkStealingPool* pool) {
    WC_ASSERT(pool);

    while (wc_atomic_u64_load(pool->total_tasks_submitted) >
           wc_atomic_u64_load(pool->total_tasks_completed)) {
        // Process tasks on main thread while waiting
        wc_pool_process_tasks(pool, 1);

        // Small delay to avoid busy waiting
        wc_cpu_pause();
    }
}

void wc_pool_process_tasks(WC_WorkStealingPool* pool, uint32_t max_tasks) {
    WC_ASSERT(pool);

    WC_WorkerThread* main_worker = &pool->workers[0];
    if (!main_worker) return;

    uint32_t processed = 0;

    while (processed < max_tasks) {
        WC_Task* task = NULL;

        // Try local queue first
        task = wc_deque_pop_bottom(main_worker->local_queue);
        if (!task) {
            // Try global queues
            task = wc_deque_steal_top(pool->high_priority_queue);
            if (!task) {
                task = wc_deque_steal_top(pool->global_queue);
            }
        }

        if (!task) {
            break; // No more tasks
        }

        wc_pool_execute_task(main_worker, task);
        processed++;
    }
}

//-------------------------------------------------------------------------------------------------
// Work stealing implementation
//-------------------------------------------------------------------------------------------------

WC_Task* wc_pool_steal_work(WC_WorkerThread* thief) {
    WC_ASSERT(thief);

    WC_WorkStealingPool* pool = thief->pool;
    if (!pool->enable_work_stealing) {
        return NULL;
    }

    uint32_t attempts = 0;
    uint32_t max_attempts = pool->steal_attempts_per_round;

    while (attempts < max_attempts) {
        uint32_t victim_id = wc_pool_select_victim(thief);
        if (victim_id == thief->thread_id) {
            attempts++;
            continue; // Don't steal from ourselves
        }

        WC_WorkerThread* victim = &pool->workers[victim_id];
        WC_Task* stolen_task = wc_deque_steal_top(victim->local_queue);

        thief->steals_attempted++;
        wc_atomic_u64_fetch_add(pool->total_steal_attempts, 1);

        if (stolen_task) {
            thief->steals_succeeded++;
            wc_atomic_u64_fetch_add(pool->total_steal_successes, 1);
            return stolen_task;
        }

        attempts++;
    }

    return NULL;
}

uint32_t wc_pool_select_victim(WC_WorkerThread* thief) {
    WC_ASSERT(thief);

    WC_WorkStealingPool* pool = thief->pool;

    if (pool->enable_numa_awareness) {
        return wc_pool_select_numa_victim(thief);
    }

    // Simple random victim selection
    uint32_t victim_id = wc_random_next(&thief->random_state) % pool->worker_count;

    // Avoid selecting ourselves
    if (victim_id == thief->thread_id) {
        victim_id = (victim_id + 1) % pool->worker_count;
    }

    return victim_id;
}

void wc_pool_execute_task(WC_WorkerThread* worker, WC_Task* task) {
    WC_ASSERT(worker && task);

    // Update task state
    wc_task_set_state(task, WC_TASK_RUNNING);
    wc_task_set_started_time(task, wc_get_time_ns());
    wc_task_set_worker_id(task, worker->thread_id);

    // Set current task
    worker->current_task = task;
    worker->task_start_time = wc_task_get_started_time(task);

    // Execute the task function
    WC_TaskFunction function = wc_task_get_function(task);
    if (function) {
        void* data = wc_task_get_data(task);
        function(data);
    }

    // Update completion time
    wc_task_set_completed_time(task, wc_get_time_ns());
    wc_task_set_state(task, WC_TASK_COMPLETED);

    // Update statistics
    worker->tasks_executed++;
    wc_atomic_u64_fetch_add(worker->pool->total_tasks_completed, 1);

    // Handle task completion (dependencies, etc.)
    wc_task_complete_internal(task);

    // Clear current task
    worker->current_task = NULL;
}

//-------------------------------------------------------------------------------------------------
// Worker management
//-------------------------------------------------------------------------------------------------

WC_WorkerThread* wc_pool_get_current_worker(void) {
    return (WC_WorkerThread*)SDL_GetTLS(&tls_current_worker);
}

WC_WorkerThread* wc_pool_get_worker(WC_WorkStealingPool* pool, uint32_t worker_id) {
    WC_ASSERT(pool);

    if (worker_id >= pool->worker_count) {
        return NULL;
    }

    return &pool->workers[worker_id];
}

void wc_pool_wake_workers(WC_WorkStealingPool* pool, uint32_t count) {
    WC_ASSERT(pool);

    if (count == 0) return;

    SDL_LockMutex(pool->sleep_mutex);

    uint64_t sleeping = wc_atomic_u64_load(pool->sleeping_workers);
    uint32_t to_wake = (count > sleeping) ? (uint32_t)sleeping : count;

    if (to_wake > 0) {
        SDL_BroadcastCondition(pool->wake_condition);
    }

    SDL_UnlockMutex(pool->sleep_mutex);
}

static void wc_pool_worker_sleep(WC_WorkerThread* worker) {
    WC_ASSERT(worker);

    WC_WorkStealingPool* pool = worker->pool;

    wc_atomic_u64_fetch_add(pool->sleeping_workers, 1);

    SDL_LockMutex(pool->sleep_mutex);

    // Check again if we should sleep (work might have arrived)
    if (wc_deque_is_empty(worker->local_queue) &&
        wc_deque_is_empty(pool->global_queue) &&
        wc_deque_is_empty(pool->high_priority_queue) &&
        !wc_atomic_bool_load(pool->shutdown)) {

        uint64_t sleep_start = wc_get_time_ns();
        SDL_WaitCondition(pool->wake_condition, pool->sleep_mutex);
        worker->idle_time += wc_get_time_ns() - sleep_start;
    }

    SDL_UnlockMutex(pool->sleep_mutex);

    wc_atomic_u64_fetch_sub(pool->sleeping_workers, 1);
}

//-------------------------------------------------------------------------------------------------
// Worker getters
//-------------------------------------------------------------------------------------------------

uint32_t wc_worker_get_id(const WC_WorkerThread* worker) {
    return worker ? worker->thread_id : UINT32_MAX;
}

uint64_t wc_worker_get_tasks_executed(const WC_WorkerThread* worker) {
    return worker ? worker->tasks_executed : 0;
}

uint64_t wc_worker_get_steals_attempted(const WC_WorkerThread* worker) {
    return worker ? worker->steals_attempted : 0;
}

uint64_t wc_worker_get_steals_succeeded(const WC_WorkerThread* worker) {
    return worker ? worker->steals_succeeded : 0;
}

WC_Task* wc_worker_get_current_task(const WC_WorkerThread* worker) {
    return worker ? worker->current_task : NULL;
}

//-------------------------------------------------------------------------------------------------
// Global pool interface
//-------------------------------------------------------------------------------------------------

WC_WorkStealingPool* wc_get_global_pool(void) {
    return g_global_pool_initialized ? g_global_pool : NULL;
}

int wc_init_global_pool(void) {
    if (g_global_pool_initialized) {
        return 0; // Already initialized
    }

    uint32_t cpu_count = wc_get_cpu_count();
    uint32_t worker_count = (cpu_count > 1) ? cpu_count - 1 : 1; // Leave one core for main thread

    g_global_pool = wc_pool_create(worker_count);
    if (!g_global_pool) {
        return -1;
    }

    g_global_pool_initialized = true;
    return 0;
}

void wc_shutdown_global_pool(void) {
    if (g_global_pool_initialized) {
        wc_pool_destroy(g_global_pool);
        g_global_pool = NULL;
        g_global_pool_initialized = false;
    }
}

//-------------------------------------------------------------------------------------------------
// NUMA stubs (implement these based on your requirements)
//-------------------------------------------------------------------------------------------------

static void wc_pool_numa_init(WC_WorkStealingPool* pool) {
    // TODO: Implement NUMA initialization
    (void)pool;
}

static uint32_t wc_pool_select_numa_victim(WC_WorkerThread* thief) {
    // For now, just use random selection
    return wc_pool_select_victim(thief);
}

//-------------------------------------------------------------------------------------------------
// Statistics and configuration (stubs for now)
//-------------------------------------------------------------------------------------------------

WC_PoolStats wc_pool_get_stats(WC_WorkStealingPool* pool) {
    WC_PoolStats stats = {0};
    if (!pool) return stats;

    stats.worker_count = pool->worker_count;
    stats.active_workers = (uint32_t)wc_atomic_u64_load(pool->active_workers);
    stats.sleeping_workers = (uint32_t)wc_atomic_u64_load(pool->sleeping_workers);
    stats.total_tasks_submitted = wc_atomic_u64_load(pool->total_tasks_submitted);
    stats.total_tasks_completed = wc_atomic_u64_load(pool->total_tasks_completed);
    stats.total_tasks_pending = stats.total_tasks_submitted - stats.total_tasks_completed;
    stats.total_steal_attempts = wc_atomic_u64_load(pool->total_steal_attempts);
    stats.total_steal_successes = wc_atomic_u64_load(pool->total_steal_successes);

    if (stats.total_steal_attempts > 0) {
        stats.overall_steal_success_rate = (double)stats.total_steal_successes /
                                          (double)stats.total_steal_attempts;
    }

    stats.global_queue_size = (uint32_t)wc_deque_size(pool->global_queue);
    stats.high_priority_queue_size = (uint32_t)wc_deque_size(pool->high_priority_queue);

    return stats;
}

void wc_pool_get_load_stats(WC_WorkStealingPool* pool, WC_LoadBalanceStats* stats) {
    if (!pool || !stats) return;

    for (uint32_t i = 0; i < pool->worker_count; i++) {
        WC_WorkerThread* worker = &pool->workers[i];
        stats[i].worker_id = worker->thread_id;
        stats[i].queue_size = (uint32_t)wc_deque_size(worker->local_queue);
        stats[i].tasks_executed = (uint32_t)worker->tasks_executed;
        stats[i].steals_attempted = (uint32_t)worker->steals_attempted;
        stats[i].steals_succeeded = (uint32_t)worker->steals_succeeded;

        if (stats[i].steals_attempted > 0) {
            stats[i].steal_success_rate = (double)stats[i].steals_succeeded /
                                         (double)stats[i].steals_attempted;
        }
    }
}

void wc_pool_reset_stats(WC_WorkStealingPool* pool) {
    if (!pool) return;

    wc_atomic_u64_store(pool->total_tasks_submitted, 0);
    wc_atomic_u64_store(pool->total_tasks_completed, 0);
    wc_atomic_u64_store(pool->total_steal_attempts, 0);
    wc_atomic_u64_store(pool->total_steal_successes, 0);

    for (uint32_t i = 0; i < pool->worker_count; i++) {
        WC_WorkerThread* worker = &pool->workers[i];
        worker->tasks_executed = 0;
        worker->local_pushes = 0;
        worker->local_pops = 0;
        worker->steals_attempted = 0;
        worker->steals_succeeded = 0;
        worker->idle_time = 0;
    }
}

void wc_pool_print_stats(WC_WorkStealingPool* pool) {
    if (!pool) return;

    WC_PoolStats stats = wc_pool_get_stats(pool);

    SDL_Log("Thread Pool Statistics:");
    SDL_Log("  Workers: %u active, %u sleeping, %u total",
            stats.active_workers, stats.sleeping_workers, stats.worker_count);
    SDL_Log("  Tasks: %" SDL_PRIu64 " submitted, %" SDL_PRIu64 " completed, %" SDL_PRIu64 " pending",
            stats.total_tasks_submitted, stats.total_tasks_completed, stats.total_tasks_pending);
    SDL_Log("  Work Stealing: %" SDL_PRIu64 " attempts, %" SDL_PRIu64 " successes (%.2f%% success rate)",
            stats.total_steal_attempts, stats.total_steal_successes,
            stats.overall_steal_success_rate * 100.0);
    SDL_Log("  Queue sizes: Global=%u, High Priority=%u",
            stats.global_queue_size, stats.high_priority_queue_size);
}

void wc_pool_configure(WC_WorkStealingPool* pool, const WC_PoolConfig* config) {
    if (!pool || !config) return;

    pool->max_idle_spins = config->max_idle_spins;
    pool->steal_attempts_per_round = config->steal_attempts_per_round;
    pool->enable_work_stealing = config->enable_work_stealing;
    pool->enable_numa_awareness = config->enable_numa_awareness;
}

WC_PoolConfig wc_pool_get_config(const WC_WorkStealingPool* pool) {
    WC_PoolConfig config = {0};
    if (!pool) return config;

    config.max_idle_spins = pool->max_idle_spins;
    config.steal_attempts_per_round = pool->steal_attempts_per_round;
    config.enable_work_stealing = pool->enable_work_stealing;
    config.enable_numa_awareness = pool->enable_numa_awareness;

    // Set default values for unimplemented features
    config.local_queue_capacity = 256;
    config.global_queue_capacity = 1024;
    config.enable_load_balancing = false;
    config.enable_statistics = true;
    config.load_balance_threshold = 8;
    config.load_balance_interval_ms = 100;

    return config;
}