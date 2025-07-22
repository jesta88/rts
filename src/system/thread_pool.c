#include "thread_pool.h"

#include "atomic.h"
#include "debug.h"
#include "deque.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <stdlib.h>
#include <string.h>

//-------------------------------------------------------------------------------------------------
// Thread-local storage
//-------------------------------------------------------------------------------------------------

static __thread WC_WorkerThread* tls_current_worker = NULL;

//-------------------------------------------------------------------------------------------------
// Global pool instance
//-------------------------------------------------------------------------------------------------

static WC_WorkStealingPool g_global_pool = {0};
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
// Worker thread main loop
//-------------------------------------------------------------------------------------------------

// Worker thread main loop
//-------------------------------------------------------------------------------------------------

static int worker_thread_main(void* arg) {
    WC_WorkerThread* worker = (WC_WorkerThread*)arg;
    WC_WorkStealingPool* pool = worker->pool;

    // Set thread-local storage
    tls_current_worker = worker;

    // Set thread name for debugging
    char thread_name[32];
    SDL_snprintf(thread_name, sizeof(thread_name), "Worker-%u", worker->thread_id);
    SDL_SetCurrentThreadName(thread_name);

    // Pin to CPU core for better cache locality
    if (pool->enable_numa_awareness && worker->thread_id > 0) {
        wc_pin_thread_to_core(worker->thread_id - 1); // -1 because thread_id 0 is main thread
    }

    // Initialize random state with unique seed
    worker->random_state = (uint32_t)(worker->thread_id * 0x9e3779b9u + (uint32_t)wc_get_time_ns());

    wc_atomic_bool_store(&worker->active, true);
    wc_atomic_fetch_add(&pool->active_workers, 1);

    uint32_t idle_spins = 0;

    while (!wc_atomic_bool_load(&pool->shutdown)) {
        WC_Task* task = NULL;

        // Phase 1: Try to get work from local queue
        task = wc_deque_pop_bottom(&worker->local_queue);
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
                task = wc_deque_steal_top(&pool->high_priority_queue);
                if (!task) {
                    task = wc_deque_steal_top(&pool->global_queue);
                }

                if (task) {
                    idle_spins = 0;
                } else {
                    // No work found - idle handling
                    idle_spins++;

                    if (idle_spins < pool->max_idle_spins) {
                        // Busy wait with CPU pause
                        WC_CPU_PAUSE();
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

    wc_atomic_bool_store(&worker->active, false);
    wc_atomic_fetch_sub(&pool->active_workers, 1);

    return 0;
}

//-------------------------------------------------------------------------------------------------
// Pool management
//-------------------------------------------------------------------------------------------------

int wc_pool_init(WC_WorkStealingPool* pool, uint32_t worker_count) {
    WC_ASSERT(pool);
    WC_ASSERT(worker_count > 0 && worker_count <= 64);

    memset(pool, 0, sizeof(WC_WorkStealingPool));

    // Default configuration
    pool->max_idle_spins = 1000;
    pool->steal_attempts_per_round = 4;
    pool->enable_work_stealing = true;
    pool->enable_numa_awareness = true;

    // Initialize synchronization primitives
    pool->sleep_mutex = SDL_CreateMutex();
    if (!pool->sleep_mutex) {
        return -1;
    }

    pool->wake_condition = SDL_CreateCondition();
    if (!pool->wake_condition) {
        SDL_DestroyMutex(pool->sleep_mutex);
        return -1;
    }

    // Initialize global queues
    if (wc_deque_init(&pool->global_queue, 1024) != 0) {
        goto cleanup;
    }
    if (wc_deque_init(&pool->high_priority_queue, 512) != 0) {
        goto cleanup;
    }

    // Allocate worker array (+1 for main thread at index 0)
    pool->worker_count = worker_count + 1;
    pool->workers = wc_aligned_alloc(pool->worker_count * sizeof(WC_WorkerThread), 64);
    if (!pool->workers) {
        goto cleanup;
    }

    // Initialize workers
    for (uint32_t i = 0; i < pool->worker_count; i++) {
        WC_WorkerThread* worker = &pool->workers[i];
        memset(worker, 0, sizeof(WC_WorkerThread));

        worker->thread_id = i;
        worker->pool = pool;

        // Initialize local queue
        if (wc_deque_init(&worker->local_queue, 256) != 0) {
            goto cleanup;
        }

        // Create thread arena
        worker->thread_arena = wc_malloc(sizeof(WC_Arena));
        if (!worker->thread_arena || wc_arena_init(worker->thread_arena, 64 * 1024) != 0) {
            goto cleanup;
        }

        // Create worker threads (skip index 0 which is the main thread)
        if (i > 0) {
            char thread_name[32];
            SDL_snprintf(thread_name, sizeof(thread_name), "WorkerThread-%u", i);

            worker->handle = SDL_CreateThread(worker_thread_main, thread_name, worker);
            if (!worker->handle) {
                goto cleanup;
            }
        }
    }

    // Set main thread as worker 0
    tls_current_worker = &pool->workers[0];

    // Initialize NUMA if enabled
    if (pool->enable_numa_awareness) {
        wc_pool_init_numa(pool);
    }

    return 0;

cleanup:
    wc_pool_shutdown(pool);
    return -1;
}

void wc_pool_shutdown(WC_WorkStealingPool* pool) {
    if (!pool) return;

    // Signal shutdown
    wc_atomic_bool_store(&pool->shutdown, true);

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
            wc_deque_destroy(&worker->local_queue);
            if (worker->thread_arena) {
                wc_arena_free(worker->thread_arena);
                wc_free(worker->thread_arena);
            }
        }

        // Cleanup main thread worker
        wc_deque_destroy(&pool->workers[0].local_queue);
        if (pool->workers[0].thread_arena) {
            wc_arena_free(pool->workers[0].thread_arena);
            wc_free(pool->workers[0].thread_arena);
        }

        wc_aligned_free(pool->workers, 64);
    }

    // Cleanup global queues
    wc_deque_destroy(&pool->global_queue);
    wc_deque_destroy(&pool->high_priority_queue);

    // Cleanup synchronization
    if (pool->wake_condition) {
        SDL_DestroyCondition(pool->wake_condition);
    }
    if (pool->sleep_mutex) {
        SDL_DestroyMutex(pool->sleep_mutex);
    }

    // Clear structure
    memset(pool, 0, sizeof(WC_WorkStealingPool));
}

int wc_pool_submit_task(WC_WorkStealingPool* pool, WC_Task* task) {
    WC_ASSERT(pool && task);

    wc_atomic_fetch_add(&pool->total_tasks_submitted, 1);

    // Try to submit to current worker's local queue first
    WC_WorkerThread* current = wc_pool_get_current_worker();
    if (current) {
        WC_DequeResult result = wc_deque_push_bottom(&current->local_queue, task);
        if (result == WC_DEQUE_SUCCESS) {
            current->local_pushes++;
            return 0;
        } else if (result == WC_DEQUE_RESIZE_NEEDED) {
            wc_deque_resize(&current->local_queue);
            if (wc_deque_push_bottom(&current->local_queue, task) == WC_DEQUE_SUCCESS) {
                current->local_pushes++;
                return 0;
            }
        }
    }

    // Local queue full or no current worker - use global queue
    WC_Deque* target_queue = (task->priority == WC_TASK_PRIORITY_HIGH ||
                             task->priority == WC_TASK_PRIORITY_CRITICAL)
                             ? &pool->high_priority_queue
                             : &pool->global_queue;

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

    while (wc_atomic_load(&pool->total_tasks_submitted) > wc_atomic_load(&pool->total_tasks_completed)) {
        // Process tasks on main thread while waiting
        wc_pool_process_tasks(pool, 1);

        // Small delay to avoid busy waiting
        WC_CPU_PAUSE();
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
        task = wc_deque_pop_bottom(&main_worker->local_queue);
        if (!task) {
            // Try global queues
            task = wc_deque_steal_top(&pool->high_priority_queue);
            if (!task) {
                task = wc_deque_steal_top(&pool->global_queue);
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
        WC_Task* stolen_task = wc_deque_steal_top(&victim->local_queue);

        thief->steals_attempted++;
        atomic_fetch_add(&pool->total_steal_attempts, 1);

        if (stolen_task) {
            thief->steals_succeeded++;
            atomic_fetch_add(&pool->total_steal_successes, 1);
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
    atomic_store(&task->state, WC_TASK_RUNNING);
    task->started_time = wc_get_time_ns();
    task->worker_id = worker->thread_id;

    // Set current task
    worker->current_task = task;
    worker->task_start_time = task->started_time;

    // Execute the task function
    if (task->function) {
        task->function(task->data);
    }

    // Update completion time
    task->completed_time = wc_get_time_ns();
    atomic_store(&task->state, WC_TASK_COMPLETED);

    // Update statistics
    worker->tasks_executed++;
    wc_atomic_fetch_add(&worker->pool->total_tasks_completed, 1);

    // Handle task completion (dependencies, etc.)
    wc_task_complete_internal(task);

    // Clear current task
    worker->current_task = NULL;
}

//-------------------------------------------------------------------------------------------------
// Worker management
//-------------------------------------------------------------------------------------------------

WC_WorkerThread* wc_pool_get_current_worker(void) {
    return tls_current_worker;
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

    uint64_t sleeping = wc_atomic_load(&pool->sleeping_workers);
    uint32_t to_wake = (count > sleeping) ? (uint32_t)sleeping : count;

    if (to_wake > 0) {
        SDL_BroadcastCondition(pool->wake_condition);
    }

    SDL_UnlockMutex(pool->sleep_mutex);
}

void wc_pool_worker_sleep(WC_WorkerThread* worker) {
    WC_ASSERT(worker);

    WC_WorkStealingPool* pool = worker->pool;

    wc_atomic_fetch_add(&pool->sleeping_workers, 1);

    SDL_LockMutex(pool->sleep_mutex);

    // Check again if we should sleep (work might have arrived)
    if (wc_deque_is_empty(&worker->local_queue) &&
        wc_deque_is_empty(&pool->global_queue) &&
        wc_deque_is_empty(&pool->high_priority_queue) &&
        !wc_atomic_bool_load(&pool->shutdown)) {

        uint64_t sleep_start = wc_get_time_ns();
        SDL_WaitCondition(pool->wake_condition, pool->sleep_mutex);
        worker->idle_time += wc_get_time_ns() - sleep_start;
    }

    SDL_UnlockMutex(pool->sleep_mutex);

    wc_atomic_fetch_sub(&pool->sleeping_workers, 1);
}

//-------------------------------------------------------------------------------------------------
// Global pool interface
//-------------------------------------------------------------------------------------------------

WC_WorkStealingPool* wc_get_global_pool(void) {
    return g_global_pool_initialized ? &g_global_pool : NULL;
}

int wc_init_global_pool(void) {
    if (g_global_pool_initialized) {
        return 0; // Already initialized
    }

    uint32_t cpu_count = wc_get_cpu_count();
    uint32_t worker_count = (cpu_count > 1) ? cpu_count - 1 : 1; // Leave one core for main thread

    if (wc_pool_init(&g_global_pool, worker_count) != 0) {
        return -1;
    }

    g_global_pool_initialized = true;
    return 0;
}

void wc_shutdown_global_pool(void) {
    if (g_global_pool_initialized) {
        wc_pool_shutdown(&g_global_pool);
        g_global_pool_initialized = false;
    }
}