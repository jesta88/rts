#pragma once

//-------------------------------------------------------------------------------------------------
// Work-stealing thread pool - lean header
//-------------------------------------------------------------------------------------------------

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct WC_WorkerThread WC_WorkerThread;
typedef struct WC_WorkStealingPool WC_WorkStealingPool;
typedef struct WC_Task WC_Task;
typedef struct WC_Arena WC_Arena;

typedef struct WC_LoadBalanceStats {
    uint32_t worker_id;
    uint32_t queue_size;
    uint32_t tasks_executed;
    uint32_t steals_attempted;
    uint32_t steals_succeeded;
    double steal_success_rate;
    double utilization;           // Percentage of time spent executing tasks
} WC_LoadBalanceStats;

typedef struct WC_PoolStats {
    uint32_t worker_count;
    uint32_t active_workers;
    uint32_t sleeping_workers;

    uint64_t total_tasks_submitted;
    uint64_t total_tasks_completed;
    uint64_t total_tasks_pending;

    uint64_t total_steal_attempts;
    uint64_t total_steal_successes;
    double overall_steal_success_rate;

    double avg_task_execution_time;
    double avg_task_wait_time;
    double overall_utilization;

    uint32_t global_queue_size;
    uint32_t high_priority_queue_size;
    uint32_t total_local_queue_size;
} WC_PoolStats;

typedef struct WC_PoolConfig {
    uint32_t max_idle_spins;              // Spins before worker sleeps
    uint32_t steal_attempts_per_round;    // Steal attempts before giving up
    uint32_t local_queue_capacity;        // Initial capacity of local queues
    uint32_t global_queue_capacity;       // Initial capacity of global queue

    bool enable_work_stealing;            // Can disable for debugging
    bool enable_numa_awareness;           // NUMA-aware victim selection
    bool enable_load_balancing;           // Automatic load balancing
    bool enable_statistics;               // Performance tracking overhead

    uint32_t load_balance_threshold;      // Queue size difference to trigger balancing
    uint32_t load_balance_interval_ms;    // How often to check for imbalance
} WC_PoolConfig;

//-------------------------------------------------------------------------------------------------
// Pool management
//-------------------------------------------------------------------------------------------------

WC_WorkStealingPool* wc_pool_create(uint32_t worker_count);
void wc_pool_destroy(WC_WorkStealingPool* pool);
int wc_pool_submit_task(WC_WorkStealingPool* pool, WC_Task* task);
int wc_pool_submit_batch(WC_WorkStealingPool* pool, WC_Task** tasks, uint32_t count);
void wc_pool_wait_idle(WC_WorkStealingPool* pool);
void wc_pool_process_tasks(WC_WorkStealingPool* pool, uint32_t max_tasks);

//-------------------------------------------------------------------------------------------------
// Worker management
//-------------------------------------------------------------------------------------------------

WC_WorkerThread* wc_pool_get_current_worker(void);
WC_WorkerThread* wc_pool_get_worker(WC_WorkStealingPool* pool, uint32_t worker_id);
void wc_pool_wake_workers(WC_WorkStealingPool* pool, uint32_t count);

// Worker getters (no need to expose full struct)
uint32_t wc_worker_get_id(const WC_WorkerThread* worker);
uint64_t wc_worker_get_tasks_executed(const WC_WorkerThread* worker);
uint64_t wc_worker_get_steals_attempted(const WC_WorkerThread* worker);
uint64_t wc_worker_get_steals_succeeded(const WC_WorkerThread* worker);
WC_Task* wc_worker_get_current_task(const WC_WorkerThread* worker);

//-------------------------------------------------------------------------------------------------
// Work stealing
//-------------------------------------------------------------------------------------------------

WC_Task* wc_pool_steal_work(WC_WorkerThread* thief);
uint32_t wc_pool_select_victim(WC_WorkerThread* thief);
void wc_pool_execute_task(WC_WorkerThread* worker, WC_Task* task);

//-------------------------------------------------------------------------------------------------
// Performance monitoring
//-------------------------------------------------------------------------------------------------

WC_PoolStats wc_pool_get_stats(WC_WorkStealingPool* pool);
void wc_pool_get_load_stats(WC_WorkStealingPool* pool, WC_LoadBalanceStats* stats);
void wc_pool_reset_stats(WC_WorkStealingPool* pool);
void wc_pool_print_stats(WC_WorkStealingPool* pool);

//-------------------------------------------------------------------------------------------------
// Configuration
//-------------------------------------------------------------------------------------------------

void wc_pool_configure(WC_WorkStealingPool* pool, const WC_PoolConfig* config);
WC_PoolConfig wc_pool_get_config(const WC_WorkStealingPool* pool);

//-------------------------------------------------------------------------------------------------
// Global pool instance
//-------------------------------------------------------------------------------------------------

WC_WorkStealingPool* wc_get_global_pool(void);
int wc_init_global_pool(void);
void wc_shutdown_global_pool(void);

void wc_debug_thread_pool_creation(void);