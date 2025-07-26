#pragma once

#include "common.h"

typedef enum
{
    JOB_PRIORITY_HIGH = 0,
    JOB_PRIORITY_NORMAL = 1,
    JOB_PRIORITY_LOW = 2,
    JOB_PRIORITY_COUNT = 3
} JobPriority;

typedef enum
{
    JOB_FLAG_SMALL_STACK = 0,
    JOB_FLAG_LARGE_STACK = 1
} job_flags_t;

typedef struct
{
    u64 value;
} JobHandle;

typedef struct
{
    JobHandle* handles;
    u32 capacity;
    u32 count;
} JobBatch;

#define INVALID_JOB_HANDLE ((JobHandle) {0})

typedef void (*JobFunc)(void* data);

// Initialize job system
bool job_system_init(u32 worker_count);
void job_system_shutdown(void);

// Job creation - returns handle instead of pointer
JobHandle job_create(JobFunc func, void* data);
JobHandle job_create_with_flags(JobFunc func, void* data, u8 flags);
JobHandle job_create_as_child(JobHandle parent, JobFunc func, void* data);

// Job execution using handles
void job_run(JobHandle job);
void job_wait(JobHandle job);
bool job_is_complete(JobHandle job);

// Batch operations
JobBatch job_batch_create(u32 capacity);
void job_batch_destroy(JobBatch* batch);
void job_batch_add(JobBatch* batch, JobHandle job);
void job_batch_run(JobBatch* batch, JobPriority priority);
void job_batch_wait(JobBatch* batch);

// Helper for parallel for loops
JobHandle job_parallel_for(u32 count, u32 batch_size, void (*func)(u32 start, u32 end, void* data), void* data);