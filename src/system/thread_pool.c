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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winbase.h>
#include <processthreadsapi.h>
#endif

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

// =============================================================================
// NUMA Topology Detection and Management
// =============================================================================

typedef struct WC_NumaNode {
	uint32_t* worker_ids;           // Array of worker IDs in this NUMA node
	uint32_t worker_count;          // Number of workers in this node
	uint32_t node_id;               // NUMA node identifier
	GROUP_AFFINITY affinity;        // Win32 processor group affinity
	uint64_t available_memory_kb;   // Available memory in KB
	float memory_bandwidth_gbps;    // Estimated memory bandwidth
} WC_NumaNode;

typedef struct WC_NumaTopology {
	WC_NumaNode* nodes;             // Array of NUMA nodes
	uint32_t node_count;            // Number of NUMA nodes
	uint32_t* worker_to_node;       // Map worker ID -> NUMA node ID
	uint32_t total_processors;      // Total logical processors
	bool topology_valid;            // Whether NUMA detection succeeded
} WC_NumaTopology;

// Global NUMA topology (initialized once)
static WC_NumaTopology* g_numa_topology = NULL;

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
static bool wc_pool_init_numa_topology(WC_WorkStealingPool* pool);
static uint32_t wc_pool_select_numa_victim(WC_WorkerThread* thief);
static WC_Task* wc_pool_steal_work_numa_aware(WC_WorkerThread* thief);

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
            task = wc_pool_steal_work_numa_aware(worker);
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
        wc_pool_init_numa_topology(pool);
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

#ifdef _WIN32
// Detect NUMA topology using Win32 APIs
static bool wc_detect_numa_topology_win32(WC_WorkStealingPool* pool) {
    ULONG highest_node_number = 0;

    // Get the highest NUMA node number
    if (!GetNumaHighestNodeNumber(&highest_node_number)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                   "GetNumaHighestNodeNumber failed: %lu. Treating as non-NUMA system.",
                   GetLastError());
        return false;
    }

    g_numa_topology->node_count = highest_node_number + 1;
    SDL_Log("Detected %u NUMA nodes", g_numa_topology->node_count);

    // Allocate node array
    g_numa_topology->nodes = wc_aligned_alloc(
        g_numa_topology->node_count * sizeof(WC_NumaNode), 64);
    if (!g_numa_topology->nodes) {
        return false;
    }

    uint32_t total_workers_assigned = 0;

    // For each NUMA node, get processor affinity and available memory
    for (ULONG node_idx = 0; node_idx <= highest_node_number; node_idx++) {
        WC_NumaNode* node = &g_numa_topology->nodes[node_idx];
        node->node_id = node_idx;
        node->worker_count = 0;

        // Get processor group affinity for this NUMA node
        GROUP_AFFINITY group_affinity = {0};
        if (!GetNumaNodeProcessorMaskEx((USHORT)node_idx, &group_affinity)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                       "GetNumaNodeProcessorMaskEx failed for node %lu: %lu",
                       node_idx, GetLastError());
            continue;
        }

        node->affinity = group_affinity;

        // Get available memory for this NUMA node
        ULONGLONG available_bytes = 0;
        if (GetNumaAvailableMemoryNodeEx((USHORT)node_idx, &available_bytes)) {
            node->available_memory_kb = available_bytes / 1024;
        } else {
            node->available_memory_kb = 0;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                       "GetNumaAvailableMemoryNodeEx failed for node %lu: %lu",
                       node_idx, GetLastError());
        }

        // Count processors in this node
        uint32_t processor_count = 0;
        KAFFINITY mask = group_affinity.Mask;
        while (mask) {
            if (mask & 1) processor_count++;
            mask >>= 1;
        }

        // Estimate memory bandwidth (rough heuristic: 50GB/s per socket for modern systems)
        node->memory_bandwidth_gbps = 50.0f * (processor_count / 16.0f); // Assume 16 cores per socket

        SDL_Log("NUMA Node %lu: %u processors, %llu KB memory, ~%.1f GB/s bandwidth",
                node_idx, processor_count, node->available_memory_kb / 1024,
                node->memory_bandwidth_gbps);

        // Allocate worker ID array (we'll populate this next)
        node->worker_ids = wc_malloc(pool->worker_count * sizeof(uint32_t));
        if (!node->worker_ids) {
            return false;
        }
    }

    // Now assign workers to NUMA nodes based on their thread affinity
    for (uint32_t worker_id = 0; worker_id < pool->worker_count; worker_id++) {
        HANDLE worker_handle = NULL;

        // Get the actual Win32 thread handle for this worker
        // Note: This requires modifying WC_WorkerThread to store the Win32 handle
        WC_WorkerThread* worker = &pool->workers[worker_id];
        if (worker->handle) {
            // Assuming worker->handle is the SDL_Thread*, we need to get Win32 handle
            // SDL3 doesn't expose this directly, so we'll use GetCurrentThread() as fallback
            worker_handle = GetCurrentThread();
        }

        if (!worker_handle) {
            // Fallback: distribute workers round-robin across NUMA nodes
            uint32_t assigned_node = worker_id % g_numa_topology->node_count;
            WC_NumaNode* node = &g_numa_topology->nodes[assigned_node];
            node->worker_ids[node->worker_count++] = worker_id;
            g_numa_topology->worker_to_node[worker_id] = assigned_node;

            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                        "Worker %u assigned to NUMA node %u (round-robin)",
                        worker_id, assigned_node);
            continue;
        }

        // Get thread's current processor number
        PROCESSOR_NUMBER proc_number = {0};
        if (GetThreadIdealProcessorEx(worker_handle, &proc_number)) {
            // Find which NUMA node this processor belongs to
            USHORT node_number = 0;
            if (GetNumaProcessorNodeEx(&proc_number, &node_number)) {
                if (node_number < g_numa_topology->node_count) {
                    WC_NumaNode* node = &g_numa_topology->nodes[node_number];
                    node->worker_ids[node->worker_count++] = worker_id;
                    g_numa_topology->worker_to_node[worker_id] = node_number;
                    total_workers_assigned++;

                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                                "Worker %u assigned to NUMA node %u (processor-based)",
                                worker_id, node_number);
                    continue;
                }
            }
        }

        // Fallback if processor detection failed
        uint32_t assigned_node = worker_id % g_numa_topology->node_count;
        WC_NumaNode* node = &g_numa_topology->nodes[assigned_node];
        node->worker_ids[node->worker_count++] = worker_id;
        g_numa_topology->worker_to_node[worker_id] = assigned_node;

        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                    "Worker %u assigned to NUMA node %u (fallback)",
                    worker_id, assigned_node);
    }

    // Log final topology
    SDL_Log("NUMA topology assignment complete:");
    for (uint32_t i = 0; i < g_numa_topology->node_count; i++) {
        WC_NumaNode* node = &g_numa_topology->nodes[i];
        SDL_Log("  Node %u: %u workers, %llu MB memory",
                i, node->worker_count, node->available_memory_kb / 1024);
    }

    return true;
}

// Set thread affinity to specific NUMA node
static bool wc_set_thread_numa_affinity(HANDLE thread_handle, uint32_t numa_node) {
    if (numa_node >= g_numa_topology->node_count) {
        return false;
    }

    WC_NumaNode* node = &g_numa_topology->nodes[numa_node];

    // Set processor group affinity
    if (!SetThreadGroupAffinity(thread_handle, &node->affinity, NULL)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                   "SetThreadGroupAffinity failed for NUMA node %u: %lu",
                   numa_node, GetLastError());
        return false;
    }

    // Set NUMA preferred node for memory allocations
    if (!SetThreadIdealProcessorEx(thread_handle, NULL, NULL)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                   "SetThreadIdealProcessorEx failed for NUMA node %u: %lu",
                   numa_node, GetLastError());
    }

    return true;
}

#endif // _WIN32

// =============================================================================
// NUMA Topology Initialization
// =============================================================================

static bool wc_pool_init_numa_topology(WC_WorkStealingPool* pool) {
    if (g_numa_topology) return true; // Already initialized

    g_numa_topology = wc_aligned_alloc(sizeof(WC_NumaTopology), 64);
    if (!g_numa_topology) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to allocate NUMA topology");
        return false;
    }

    memset(g_numa_topology, 0, sizeof(WC_NumaTopology));

    // Allocate worker-to-node mapping
    g_numa_topology->worker_to_node = wc_malloc(pool->worker_count * sizeof(uint32_t));
    if (!g_numa_topology->worker_to_node) {
        wc_aligned_free(g_numa_topology, 64);
        g_numa_topology = NULL;
        return false;
    }

#ifdef _WIN32
    // Use Win32 NUMA detection
    if (wc_detect_numa_topology_win32(pool)) {
        g_numa_topology->topology_valid = true;
        SDL_Log("NUMA topology detection successful");
        return true;
    }
#endif

    // Fallback: create single node with all workers
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
               "NUMA detection failed, using single-node fallback");

    g_numa_topology->node_count = 1;
    g_numa_topology->nodes = wc_aligned_alloc(sizeof(WC_NumaNode), 64);

    if (!g_numa_topology->nodes) {
        wc_free(g_numa_topology->worker_to_node);
        wc_aligned_free(g_numa_topology, 64);
        g_numa_topology = NULL;
        return false;
    }

    // Initialize single node with all workers
    WC_NumaNode* node = &g_numa_topology->nodes[0];
    node->node_id = 0;
    node->worker_count = pool->worker_count;
    node->available_memory_kb = 8 * 1024 * 1024; // Assume 8GB
    node->memory_bandwidth_gbps = 25.0f; // Conservative estimate
    node->worker_ids = wc_malloc(pool->worker_count * sizeof(uint32_t));

    if (!node->worker_ids) {
        wc_free(g_numa_topology->nodes);
        wc_free(g_numa_topology->worker_to_node);
        wc_aligned_free(g_numa_topology, 64);
        g_numa_topology = NULL;
        return false;
    }

    for (uint32_t i = 0; i < pool->worker_count; i++) {
        node->worker_ids[i] = i;
        g_numa_topology->worker_to_node[i] = 0;
    }

    g_numa_topology->topology_valid = true;
    SDL_Log("Single-node NUMA topology initialized with %u workers", pool->worker_count);
    return true;
}

// =============================================================================
// Optimal NUMA-Aware Victim Selection
// =============================================================================

// Core random victim selection (non-recursive)
static uint32_t wc_pool_select_random_victim(WC_WorkerThread* thief) {
    WC_ASSERT(thief);
    WC_WorkStealingPool* pool = thief->pool;

    if (pool->worker_count <= 1) {
        return thief->thread_id; // No other workers to steal from
    }

    // Generate random victim different from ourselves
    uint32_t victim_id;
    do {
        victim_id = wc_random_next(&thief->random_state) % pool->worker_count;
    } while (victim_id == thief->thread_id);

    return victim_id;
}

// NUMA-aware victim selection with three-tier strategy
static uint32_t wc_pool_select_numa_victim(WC_WorkerThread* thief) {
    WC_ASSERT(thief);

    if (!g_numa_topology || !g_numa_topology->topology_valid) {
        return wc_pool_select_random_victim(thief);
    }

    uint32_t thief_node = g_numa_topology->worker_to_node[thief->thread_id];
    WC_NumaNode* local_node = &g_numa_topology->nodes[thief_node];
    uint32_t random_val = wc_random_next(&thief->random_state) % 100;

    // Tier 1: Local NUMA node (70% probability)
    // Prioritize same-node stealing for best cache locality
    if (local_node->worker_count > 1 && random_val < 70) {
        uint32_t local_victim_idx;
        uint32_t attempts = 0;
        do {
            local_victim_idx = wc_random_next(&thief->random_state) % local_node->worker_count;
            attempts++;
        } while (local_node->worker_ids[local_victim_idx] == thief->thread_id &&
                 attempts < local_node->worker_count);

        if (attempts < local_node->worker_count) {
            return local_node->worker_ids[local_victim_idx];
        }
    }

    // Tier 2: Adjacent NUMA nodes with high memory bandwidth (25% probability)
    // Target nodes with good memory performance for compute-intensive tasks
    if (g_numa_topology->node_count > 1 && random_val < 95) {
        // Find the NUMA node with highest memory bandwidth (excluding local node)
        uint32_t best_remote_node = UINT32_MAX;
        float best_bandwidth = 0.0f;

        for (uint32_t i = 0; i < g_numa_topology->node_count; i++) {
            if (i != thief_node && g_numa_topology->nodes[i].worker_count > 0) {
                if (g_numa_topology->nodes[i].memory_bandwidth_gbps > best_bandwidth) {
                    best_bandwidth = g_numa_topology->nodes[i].memory_bandwidth_gbps;
                    best_remote_node = i;
                }
            }
        }

        if (best_remote_node != UINT32_MAX) {
            WC_NumaNode* remote_node = &g_numa_topology->nodes[best_remote_node];
            uint32_t remote_victim_idx = wc_random_next(&thief->random_state) % remote_node->worker_count;
            return remote_node->worker_ids[remote_victim_idx];
        }
    }

    // Tier 3: Random NUMA node (5% probability)
    // Last resort - completely random selection for load balancing
    uint32_t random_node;
    do {
        random_node = wc_random_next(&thief->random_state) % g_numa_topology->node_count;
    } while (random_node == thief_node || g_numa_topology->nodes[random_node].worker_count == 0);

    WC_NumaNode* random_remote_node = &g_numa_topology->nodes[random_node];
    uint32_t random_victim_idx = wc_random_next(&thief->random_state) % random_remote_node->worker_count;
    return random_remote_node->worker_ids[random_victim_idx];
}

// Main victim selection function (FIXED - no recursion)
uint32_t wc_pool_select_victim(WC_WorkerThread* thief) {
    WC_ASSERT(thief);
    WC_WorkStealingPool* pool = thief->pool;

    // Initialize NUMA topology on first use
    if (pool->enable_numa_awareness && !g_numa_topology) {
        wc_pool_init_numa_topology(pool);
    }

    if (pool->enable_numa_awareness && g_numa_topology && g_numa_topology->topology_valid) {
        return wc_pool_select_numa_victim(thief);
    }

    return wc_pool_select_random_victim(thief);
}

// =============================================================================
// Enhanced Work Stealing with NUMA Awareness
// =============================================================================

WC_Task* wc_pool_steal_work_numa_aware(WC_WorkerThread* thief) {
    WC_ASSERT(thief);

    WC_WorkStealingPool* pool = thief->pool;
    if (!pool->enable_work_stealing) {
        return NULL;
    }

    uint32_t attempts = 0;
    uint32_t max_attempts = pool->steal_attempts_per_round;
    uint32_t successful_steals = 0;

    // Track victim selection performance
    uint32_t local_attempts = 0, remote_attempts = 0;

    while (attempts < max_attempts) {
        uint32_t victim_id = wc_pool_select_victim(thief);

        // Safety checks
        if (victim_id >= pool->worker_count || victim_id == thief->thread_id) {
            attempts++;
            continue;
        }

        // Track local vs remote stealing
        if (g_numa_topology && g_numa_topology->topology_valid) {
            uint32_t thief_node = g_numa_topology->worker_to_node[thief->thread_id];
            uint32_t victim_node = g_numa_topology->worker_to_node[victim_id];

            if (thief_node == victim_node) {
                local_attempts++;
            } else {
                remote_attempts++;
            }
        }

        WC_WorkerThread* victim = &pool->workers[victim_id];
        WC_Task* stolen_task = wc_deque_steal_top(victim->local_queue);

        thief->steals_attempted++;
        wc_atomic_u64_fetch_add(pool->total_steal_attempts, 1);

        if (stolen_task) {
            thief->steals_succeeded++;
            wc_atomic_u64_fetch_add(pool->total_steal_successes, 1);
            successful_steals++;

            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                        "Worker %u stole from %u (attempt %u, %u local, %u remote)",
                        thief->thread_id, victim_id, attempts + 1,
                        local_attempts, remote_attempts);

            return stolen_task;
        }

        attempts++;

        // Adaptive backoff based on NUMA distance
        if (attempts % 4 == 0) {
            uint32_t pause_cycles = 1;

            if (g_numa_topology && g_numa_topology->topology_valid) {
                uint32_t thief_node = g_numa_topology->worker_to_node[thief->thread_id];
                uint32_t victim_node = g_numa_topology->worker_to_node[victim_id];

                // Longer pause for remote NUMA access
                if (thief_node != victim_node) {
                    pause_cycles = 4;
                }
            }

            for (uint32_t i = 0; i < pause_cycles; i++) {
                wc_cpu_pause();
            }
        }
    }

    return NULL;
}

// =============================================================================
// Memory cleanup
// =============================================================================

void wc_pool_cleanup_numa_topology(void) {
	if (g_numa_topology) {
		if (g_numa_topology->nodes) {
			for (uint32_t i = 0; i < g_numa_topology->node_count; i++) {
				if (g_numa_topology->nodes[i].worker_ids) {
					wc_free(g_numa_topology->nodes[i].worker_ids);
				}
			}
			wc_aligned_free(g_numa_topology->nodes, 64);
		}

		if (g_numa_topology->worker_to_node) {
			wc_free(g_numa_topology->worker_to_node);
		}

		wc_aligned_free(g_numa_topology, 64);
		g_numa_topology = NULL;

		SDL_Log("NUMA topology cleaned up");
	}
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
	wc_pool_init_numa_topology(g_global_pool);

    g_global_pool_initialized = true;
    return 0;
}

void wc_shutdown_global_pool(void) {
    if (g_global_pool_initialized) {
    	wc_pool_cleanup_numa_topology();
        wc_pool_destroy(g_global_pool);
        g_global_pool = NULL;
        g_global_pool_initialized = false;
    }
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

// =============================================================================
// Thread Pool Integration
// =============================================================================

// Initialize worker threads with NUMA affinity
int wc_pool_init_numa_workers(WC_WorkStealingPool* pool) {
	if (!wc_pool_init_numa_topology(pool)) {
		return -1;
	}

#ifdef _WIN32
	// Set affinity for existing worker threads
	for (uint32_t i = 1; i < pool->worker_count; i++) { // Skip main thread (index 0)
		WC_WorkerThread* worker = &pool->workers[i];
		if (worker->handle && g_numa_topology && g_numa_topology->topology_valid) {
			uint32_t numa_node = g_numa_topology->worker_to_node[i];

			// Get Win32 thread handle from SDL thread
			// Note: SDL3 doesn't expose this directly, this is conceptual
			HANDLE win32_handle = GetCurrentThread(); // Placeholder

			if (wc_set_thread_numa_affinity(win32_handle, numa_node)) {
				SDL_Log("Set NUMA affinity for worker %u to node %u", i, numa_node);
			}
		}
	}
#endif

	return 0;
}

// =============================================================================
// Performance Monitoring
// =============================================================================

typedef struct WC_NumaStats {
	uint64_t local_steals;
	uint64_t remote_steals;
	uint64_t failed_local_steals;
	uint64_t failed_remote_steals;
	double local_success_rate;
	double remote_success_rate;
	double numa_efficiency; // Higher is better (more local steals)
} WC_NumaStats;

WC_NumaStats wc_pool_get_numa_stats(WC_WorkStealingPool* pool) {
	WC_NumaStats stats = {0};

	if (!g_numa_topology || !g_numa_topology->topology_valid) {
		return stats;
	}

	// Aggregate stats from all workers
	for (uint32_t i = 0; i < pool->worker_count; i++) {
		WC_WorkerThread* worker = &pool->workers[i];
		// Note: We'd need to add local/remote steal tracking to worker stats
		stats.local_steals += worker->steals_succeeded; // Placeholder
		stats.remote_steals += 0; // Would track separately
	}

	// Calculate success rates and efficiency
	uint64_t total_local = stats.local_steals + stats.failed_local_steals;
	uint64_t total_remote = stats.remote_steals + stats.failed_remote_steals;

	if (total_local > 0) {
		stats.local_success_rate = (double)stats.local_steals / total_local;
	}

	if (total_remote > 0) {
		stats.remote_success_rate = (double)stats.remote_steals / total_remote;
	}

	uint64_t total_steals = stats.local_steals + stats.remote_steals;
	if (total_steals > 0) {
		stats.numa_efficiency = (double)stats.local_steals / total_steals;
	}

	return stats;
}