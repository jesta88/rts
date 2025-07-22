#pragma once

//-------------------------------------------------------------------------------------------------
// Task system
//-------------------------------------------------------------------------------------------------

#include <stdint.h>
#include <stdbool.h>

// Forward declarations - no complex includes needed
typedef struct WC_Task WC_Task;
typedef struct WC_TaskGroup WC_TaskGroup;

typedef enum WC_TaskState {
    WC_TASK_PENDING,      // Waiting for dependencies
    WC_TASK_READY,        // Ready to execute
    WC_TASK_RUNNING,      // Currently executing
    WC_TASK_COMPLETED,    // Finished execution
    WC_TASK_CANCELLED     // Cancelled before execution
} WC_TaskState;

typedef enum WC_TaskPriority {
    WC_TASK_PRIORITY_CRITICAL = 0,  // Never steal these (NUMA-pinned)
    WC_TASK_PRIORITY_HIGH = 1,      // Steal locally first
    WC_TASK_PRIORITY_NORMAL = 2,    // Normal work stealing
    WC_TASK_PRIORITY_LOW = 3        // Steal these first
} WC_TaskPriority;

typedef enum WC_TaskYield {
    WC_TASK_CONTINUE,    // Keep running this task
    WC_TASK_YIELD,       // Reschedule this task for later
    WC_TASK_COMPLETE     // Task is finished
} WC_TaskYield;

// Task function signatures
typedef void (*WC_TaskFunction)(void* data);
typedef WC_TaskYield (*WC_CooperativeTaskFunction)(void* data);

typedef struct WC_TaskStats {
    uint64_t total_tasks_created;
    uint64_t total_tasks_completed;
    uint64_t total_tasks_cancelled;
    uint64_t total_execution_time;
    uint64_t total_wait_time;
    double avg_execution_time;
    double avg_wait_time;
    uint32_t active_tasks;
    uint32_t pending_tasks;
} WC_TaskStats;

typedef struct WC_TaskPerfInfo {
    uint64_t creation_time;
    uint64_t start_time;
    uint64_t completion_time;
    uint64_t execution_duration;
    uint64_t wait_duration;
    uint32_t worker_id;
    uint32_t dependency_count;
} WC_TaskPerfInfo;

//-------------------------------------------------------------------------------------------------
// Task creation and management
//-------------------------------------------------------------------------------------------------

// Create tasks
WC_Task* wc_task_create(WC_TaskFunction function, void* data);
WC_Task* wc_task_create_advanced(WC_TaskFunction function, void* data,
                                 WC_TaskPriority priority, uint32_t affinity_mask);
WC_Task* wc_task_create_cooperative(WC_CooperativeTaskFunction function, void* data);

// Destroy tasks
void wc_task_destroy(WC_Task* task);

// Task dependencies
int wc_task_add_dependency(WC_Task* dependent_task, WC_Task* dependency_task);

// Submit tasks
int wc_task_submit(WC_Task* task);
int wc_task_submit_batch(WC_Task** tasks, uint32_t count);

// Wait for tasks
void wc_task_wait(WC_Task* task);
void wc_task_wait_all(WC_Task** tasks, uint32_t count);

//-------------------------------------------------------------------------------------------------
// Task groups
//-------------------------------------------------------------------------------------------------

WC_TaskGroup* wc_task_group_create(uint32_t estimated_task_count);
void wc_task_group_destroy(WC_TaskGroup* group);
int wc_task_group_add(WC_TaskGroup* group, WC_Task* task);
void wc_task_group_set_continuation(WC_TaskGroup* group, WC_Task* continuation);
void wc_task_group_wait(WC_TaskGroup* group);
int wc_task_group_submit(WC_TaskGroup* group);

//-------------------------------------------------------------------------------------------------
// Hierarchical tasks
//-------------------------------------------------------------------------------------------------

WC_Task* wc_task_spawn_child(WC_Task* parent, WC_TaskFunction function, void* data);
int wc_task_spawn_children(WC_Task* parent, WC_TaskFunction function,
                          void** data_array, uint32_t count, WC_Task** out_tasks);

//-------------------------------------------------------------------------------------------------
// Task utilities
//-------------------------------------------------------------------------------------------------

WC_Task* wc_task_get_current(void);
bool wc_task_is_executing(void);
uint32_t wc_task_get_worker_id(void);
void wc_task_yield(void);

// Task state queries
WC_TaskState wc_task_get_state(const WC_Task* task);
uint32_t wc_task_get_priority(const WC_Task* task);
void wc_task_set_priority(WC_Task* task, uint32_t priority);

//-------------------------------------------------------------------------------------------------
// Performance monitoring
//-------------------------------------------------------------------------------------------------

WC_TaskStats wc_task_get_stats(void);
void wc_task_reset_stats(void);
WC_TaskPerfInfo wc_task_get_perf_info(const WC_Task* task);

//-------------------------------------------------------------------------------------------------
// Internal functions (used by work stealing pool)
//-------------------------------------------------------------------------------------------------

void wc_task_complete_internal(WC_Task* task);