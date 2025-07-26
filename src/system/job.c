#include "job.h"

#define WIN32_LEAN_AND_MEAN
#include <assert.h>
#include <intrin.h>
#include <windows.h>

// Configuration
#define MAX_JOB_COUNT 4096
#define MAX_WORKER_THREADS 16
#define CACHE_LINE_SIZE 64
#define JOB_QUEUE_SIZE 256
#define LARGE_FIBER_BIT 0x80000000
#define LARGE_FIBER_MASK (LARGE_FIBER_BIT - 1)
#define SMALL_FIBER_STACK_SIZE (16 * 1024)  // 16KB for simple jobs
#define LARGE_FIBER_STACK_SIZE (256 * 1024) // 256KB for complex jobs
#define SMALL_FIBER_POOL_SIZE 128           // Many small fibers
#define LARGE_FIBER_POOL_SIZE 16            // Few large fibers

typedef struct Job
{
    JobFunc function;
    void* data;
    volatile LONG unfinished_jobs;
    u32 parent_index; // Index of parent job (MAX_JOB_COUNT if no parent)
    u32 generation;   // Generation counter for this slot
    u8 flags;
    u8 allocated; // Is this slot currently in use?
    u8 padding[CACHE_LINE_SIZE - sizeof(JobFunc) - sizeof(void*) - sizeof(LONG) - sizeof(u32) * 2 - sizeof(u8) * 2];
} Job;

// Per-thread job queue (SPSC - Single Producer Single Consumer)
typedef struct
{
    alignas(CACHE_LINE_SIZE) volatile LONG head;
    alignas(CACHE_LINE_SIZE) volatile LONG tail;
    alignas(CACHE_LINE_SIZE) Job* jobs[JOB_QUEUE_SIZE];
} JobQueue;

// Work-stealing deque for each worker
typedef struct
{
    alignas(CACHE_LINE_SIZE) volatile LONG top;
    alignas(CACHE_LINE_SIZE) volatile LONG bottom;
    alignas(CACHE_LINE_SIZE) Job* jobs[JOB_QUEUE_SIZE];
} WorkStealingQueue;

// Fiber pool for each size
typedef struct
{
    void* fibers[SMALL_FIBER_POOL_SIZE];
    volatile LONG available_mask[SMALL_FIBER_POOL_SIZE / 32]; // Bitmask for available fibers
    u32 count;
    u32 stack_size;
} SmallFiberPool;

typedef struct
{
    void* fibers[LARGE_FIBER_POOL_SIZE];
    volatile LONG available_mask; // Single 32-bit mask sufficient for 16 fibers
    u32 count;
    u32 stack_size;
} LargeFiberPool;

// Worker fiber context
typedef struct
{
    void* thread_fiber;
    void* scheduler_fiber;
    SmallFiberPool small_fibers;
    LargeFiberPool large_fibers;
    void* current_job_fiber;
    u32 spin_count;
    u32 consecutive_steals;
} WorkerFiberContext;

// Worker thread data
typedef struct
{
    HANDLE thread;
    DWORD thread_id;
    u32 worker_index;

    // Each worker has its own deque for work stealing
    WorkStealingQueue* deque;

    // Local job queues for each priority
    JobQueue priority_queues[JOB_PRIORITY_COUNT];

    // Fiber context for scheduling
    WorkerFiberContext fiber_context;

    // Performance counters
    alignas(CACHE_LINE_SIZE) u64 jobs_executed;
    alignas(CACHE_LINE_SIZE) u64 jobs_stolen;

    char padding[CACHE_LINE_SIZE];
} WorkerThread;

// Job pool with free list and generation counters
typedef struct
{
    alignas(CACHE_LINE_SIZE) Job jobs[MAX_JOB_COUNT];
    alignas(CACHE_LINE_SIZE) volatile LONG free_indices[MAX_JOB_COUNT];
    alignas(CACHE_LINE_SIZE) volatile LONG free_head;
    alignas(CACHE_LINE_SIZE) volatile LONG free_tail;
    alignas(CACHE_LINE_SIZE) volatile LONG allocated_count;
} JobPool;

// Job system state
typedef struct
{
    WorkerThread workers[MAX_WORKER_THREADS];
    u32 worker_count;

    // Global job pool with free list
    JobPool job_pool;

    // Synchronization
    volatile LONG should_quit;

    // Main thread fiber
    void* main_fiber;
} JobSystem;

// Global job system instance
static JobSystem g_job_system;

// Thread-local storage for current worker
WAR_THREAD_LOCAL WorkerThread* tls_current_worker = NULL;

// Helper to pack/unpack job handles
static inline JobHandle make_job_handle(u32 index, u32 generation)
{
    return (JobHandle) {((u64) generation << 32) | index};
}

static inline void unpack_job_handle(JobHandle handle, u32* index, u32* generation)
{
    *index = (u32) (handle.value & 0xFFFFFFFF);
    *generation = (u32) (handle.value >> 32);
}

static inline bool is_valid_handle(JobHandle handle)
{
    return handle.value != 0;
}

// Get job from handle with generation check
static inline Job* get_job_from_handle(JobHandle handle)
{
    if (!is_valid_handle(handle))
        return NULL;

    u32 index, generation;
    unpack_job_handle(handle, &index, &generation);

    if (index >= MAX_JOB_COUNT)
        return NULL;

    Job* job = &g_job_system.job_pool.jobs[index];

    // Check generation to detect use-after-free
    if (job->generation != generation || !job->allocated)
    {
        return NULL;
    }

    return job;
}

// Initialize job pool
static void job_pool_init(JobPool* pool)
{
    pool->free_head = 0;
    pool->free_tail = 0;
    pool->allocated_count = 0;

    // Initialize all jobs and free list
    for (LONG i = 0; i < MAX_JOB_COUNT; ++i)
    {
        pool->jobs[i].generation = 1; // Start at generation 1 (0 is invalid)
        pool->jobs[i].allocated = 0;
        pool->jobs[i].parent_index = MAX_JOB_COUNT;
        pool->free_indices[i] = i;
    }
    pool->free_tail = MAX_JOB_COUNT;
}

// Allocate job from pool and return handle
static inline JobHandle job_alloc(void)
{
    JobPool* pool = &g_job_system.job_pool;

    for (;;)
    {
        LONG head = pool->free_head;
        LONG tail = pool->free_tail;

        if (head >= tail)
        {
            // Pool exhausted
            return INVALID_JOB_HANDLE;
        }

        // Try to claim a job
        LONG new_head = head + 1;
        if (InterlockedCompareExchange(&pool->free_head, new_head, head) == head)
        {
            LONG idx = pool->free_indices[head & (MAX_JOB_COUNT - 1)];
            Job* job = &pool->jobs[idx];

            // Mark as allocated
            job->allocated = 1;
            InterlockedIncrement(&pool->allocated_count);

            return make_job_handle(idx, job->generation);
        }
    }
}

// Return job to pool
static inline void job_free(Job* job)
{
    JobPool* pool = &g_job_system.job_pool;

    LONG idx = (LONG) (job - pool->jobs);

    // Increment generation to invalidate any outstanding handles
    job->generation++;
    job->allocated = 0;

    // Return to free list
    LONG tail = InterlockedIncrement(&pool->free_tail) - 1;
    pool->free_indices[tail & (MAX_JOB_COUNT - 1)] = idx;
    InterlockedDecrement(&pool->allocated_count);
}

// Push job to worker's local queue with bounds checking
static inline bool job_queue_push(JobQueue* queue, Job* job)
{
    LONG tail = queue->tail;
    LONG head = queue->head;
    LONG next = (tail + 1) & (JOB_QUEUE_SIZE - 1);

    if (next == head)
    {
        return false; // Queue full
    }

    queue->jobs[tail] = job;
    MemoryBarrier(); // Full memory fence
    queue->tail = next;
    return true;
}

// Pop job from worker's local queue
static inline Job* job_queue_pop(JobQueue* queue)
{
    LONG head = queue->head;
    LONG tail = queue->tail;

    if (head == tail)
    {
        return NULL; // Queue empty
    }

    Job* job = queue->jobs[head];
    MemoryBarrier(); // Full memory fence
    queue->head = (head + 1) & (JOB_QUEUE_SIZE - 1);
    return job;
}

// Work-stealing deque operations with proper memory ordering
static inline bool deque_push_bottom(WorkStealingQueue* deque, Job* job)
{
    LONG b = deque->bottom;
    LONG t = deque->top;

    // Check if deque is full
    if (b - t >= JOB_QUEUE_SIZE)
    {
        return false;
    }

    deque->jobs[b & (JOB_QUEUE_SIZE - 1)] = job;
    MemoryBarrier(); // Full fence to ensure job is visible before bottom update
    InterlockedExchange(&deque->bottom, b + 1);
    return true;
}

static inline Job* deque_pop_bottom(WorkStealingQueue* deque)
{
    LONG b = InterlockedDecrement(&deque->bottom);
    MemoryBarrier(); // Full fence after bottom update
    LONG t = deque->top;

    if (t <= b)
    {
        Job* job = deque->jobs[b & (JOB_QUEUE_SIZE - 1)];
        if (t == b)
        {
            // Last item, might race with steal
            if (InterlockedCompareExchange(&deque->top, t + 1, t) != t)
            {
                // Lost race
                InterlockedIncrement(&deque->bottom);
                return NULL;
            }
            InterlockedIncrement(&deque->bottom);
        }
        return job;
    }
    else
    {
        // Empty
        InterlockedIncrement(&deque->bottom);
        return NULL;
    }
}

static inline Job* deque_steal(WorkStealingQueue* deque)
{
    LONG t = deque->top;
    MemoryBarrier(); // Full fence to ensure we see latest bottom
    LONG b = deque->bottom;

    if (t < b)
    {
        Job* job = deque->jobs[t & (JOB_QUEUE_SIZE - 1)];
        MemoryBarrier(); // Ensure we read job before CAS

        if (InterlockedCompareExchange(&deque->top, t + 1, t) == t)
        {
            return job;
        }
    }
    return NULL;
}

// Get current worker thread using TLS
static inline WorkerThread* get_current_worker(void)
{
    return tls_current_worker;
}

// Initialize fiber pools
static void init_fiber_pools(WorkerFiberContext* ctx)
{
    // Initialize small fiber pool
    ctx->small_fibers.count = SMALL_FIBER_POOL_SIZE;
    ctx->small_fibers.stack_size = SMALL_FIBER_STACK_SIZE;
    for (u32 i = 0; i < SMALL_FIBER_POOL_SIZE; ++i)
    {
        ctx->small_fibers.fibers[i] = NULL; // Created on demand
        if (i < 32)
        {
            ctx->small_fibers.available_mask[0] = 0xFFFFFFFF; // All available
        }
        else
        {
            ctx->small_fibers.available_mask[i / 32] = 0xFFFFFFFF;
        }
    }

    // Initialize large fiber pool
    ctx->large_fibers.count = LARGE_FIBER_POOL_SIZE;
    ctx->large_fibers.stack_size = LARGE_FIBER_STACK_SIZE;
    ctx->large_fibers.available_mask = (1 << LARGE_FIBER_POOL_SIZE) - 1; // All available
    for (u32 i = 0; i < LARGE_FIBER_POOL_SIZE; ++i)
    {
        ctx->large_fibers.fibers[i] = NULL; // Created on demand
    }
}

// Get an available fiber from the appropriate pool
static void* acquire_fiber(WorkerFiberContext* ctx, u8 flags, u32* out_index)
{
    if (flags & JOB_FLAG_LARGE_STACK)
    {
        // Try to get a large fiber
        LONG mask = ctx->large_fibers.available_mask;
        if (mask == 0)
            return NULL; // None available

        // Find first available bit
        unsigned long index;
        _BitScanForward(&index, mask);

        // Try to claim it
        LONG new_mask = mask & ~(1 << index);
        if (InterlockedCompareExchange(&ctx->large_fibers.available_mask, new_mask, mask) == mask)
        {
            // Create fiber on demand
            if (!ctx->large_fibers.fibers[index])
            {
                ctx->large_fibers.fibers[index] = CreateFiber(LARGE_FIBER_STACK_SIZE, NULL, NULL);
            }
            *out_index = index | 0x80000000; // High bit indicates large fiber
            return ctx->large_fibers.fibers[index];
        }
    }
    else
    {
        // Try to get a small fiber
        for (u32 i = 0; i < (SMALL_FIBER_POOL_SIZE / 32); ++i)
        {
            LONG mask = ctx->small_fibers.available_mask[i];
            if (mask == 0)
                continue;

            unsigned long index;
            _BitScanForward(&index, mask);
            u32 fiber_index = i * 32 + index;

            // Try to claim it
            LONG new_mask = mask & ~(1 << index);
            if (InterlockedCompareExchange(&ctx->small_fibers.available_mask[i], new_mask, mask) == mask)
            {
                // Create fiber on demand
                if (!ctx->small_fibers.fibers[fiber_index])
                {
                    ctx->small_fibers.fibers[fiber_index] = CreateFiber(SMALL_FIBER_STACK_SIZE, NULL, NULL);
                }
                *out_index = fiber_index;
                return ctx->small_fibers.fibers[fiber_index];
            }
        }
    }

    return NULL; // No fibers available
}

// Release a fiber back to the pool
static void release_fiber(WorkerFiberContext* ctx, u32 index)
{
    if (index & 0x80000000)
    {
        // Large fiber
        u32 real_index = index & 0x7FFFFFFF;
        InterlockedOr(&ctx->large_fibers.available_mask, 1 << real_index);
    }
    else
    {
        // Small fiber
        u32 mask_index = index / 32;
        u32 bit_index = index % 32;
        InterlockedOr(&ctx->small_fibers.available_mask[mask_index], 1 << bit_index);
    }
}

// Execute a job
static void job_execute(Job* job)
{
    job->function(job->data);

    // Decrement parent's unfinished count
    if (job->parent_index < MAX_JOB_COUNT)
    {
        Job* parent = &g_job_system.job_pool.jobs[job->parent_index];
        InterlockedDecrement(&parent->unfinished_jobs);
    }

    // Decrement job's own count
    InterlockedDecrement(&job->unfinished_jobs);

    // Return job to pool
    job_free(job);
}

// Try to get a job from local queues
static Job* worker_get_job(WorkerThread* worker)
{
    // Try priority queues first
    for (int p = 0; p < JOB_PRIORITY_COUNT; ++p)
    {
        Job* job = job_queue_pop(&worker->priority_queues[p]);
        if (job)
            return job;
    }

    // Try local deque
    Job* job = deque_pop_bottom(worker->deque);
    if (job)
        return job;

    // Try stealing from other workers
    u32 victim_count = g_job_system.worker_count;

    // First try immediate neighbors (likely on same CPU complex)
    u32 left = (worker->worker_index - 1) & (victim_count - 1);
    u32 right = (worker->worker_index + 1) & (victim_count - 1);

    Job* stolen = deque_steal(g_job_system.workers[left].deque);
    if (stolen)
    {
        InterlockedIncrement64((LONG64*) &worker->jobs_stolen);
        return stolen;
    }

    stolen = deque_steal(g_job_system.workers[right].deque);
    if (stolen)
    {
        InterlockedIncrement64((LONG64*) &worker->jobs_stolen);
        return stolen;
    }

    // Try other workers
    u32 victim = worker->worker_index;
    for (u32 i = 2; i < victim_count; ++i)
    {
        victim = (victim + 1) % victim_count;
        if (victim == worker->worker_index)
            continue;

        stolen = deque_steal(g_job_system.workers[victim].deque);
        if (stolen)
        {
            InterlockedIncrement64((LONG64*) &worker->jobs_stolen);
            return stolen;
        }
    }

    return NULL;
}

// Scheduler fiber function - manages work acquisition and execution
static void CALLBACK scheduler_fiber_func(void* param)
{
    WorkerThread* worker = (WorkerThread*) param;
    WorkerFiberContext* ctx = &worker->fiber_context;

    // Spin parameters
    const u32 INITIAL_SPIN = 100;
    const u32 YIELD_CYCLES = 64;

    while (!g_job_system.should_quit)
    {
        Job* job = worker_get_job(worker);

        if (job)
        {
            // Found work - execute it
            job_execute(job);
            InterlockedIncrement64((LONG64*) &worker->jobs_executed);

            // Reset spin count since we found work
            ctx->spin_count = INITIAL_SPIN;
            ctx->consecutive_steals = 0;
        }
        else
        {
            // No work available - graduated spinning strategy
            if (ctx->spin_count > 0)
            {
                // Active spinning with pause instruction
                u32 spins = min(ctx->spin_count, YIELD_CYCLES);
                for (u32 i = 0; i < spins; ++i)
                {
                    _mm_pause();

                    // Check for work periodically during spin
                    if ((i & 7) == 7)
                    {
                        job = worker_get_job(worker);
                        if (job)
                        {
                            job_execute(job);
                            InterlockedIncrement64((LONG64*) &worker->jobs_executed);
                            ctx->spin_count = INITIAL_SPIN;
                            break;
                        }
                    }
                }

                // Decay spin count
                if (ctx->spin_count > YIELD_CYCLES)
                {
                    ctx->spin_count -= YIELD_CYCLES;
                }
                else
                {
                    ctx->spin_count = 0;
                }
            }
            else
            {
                // We've spun enough, yield to OS briefly
                // This is a very short yield - just giving other threads a chance
                SwitchToFiber(ctx->thread_fiber);

                // When we come back, do moderate spinning
                ctx->spin_count = INITIAL_SPIN / 2;
            }
        }
    }
}

// Minimal thread fiber that yields back to scheduler
static void CALLBACK thread_fiber_func(void* param)
{
    WorkerThread* worker = (WorkerThread*) param;

    // Immediately switch back to scheduler
    // This gives the OS a chance to run other threads
    SwitchToFiber(worker->fiber_context.scheduler_fiber);
}

// Worker thread function
static DWORD WINAPI worker_thread_func(void* param)
{
    WorkerThread* worker = (WorkerThread*) param;

    // Set thread-local storage
    tls_current_worker = worker;

    // Convert thread to fiber
    worker->fiber_context.thread_fiber = ConvertThreadToFiber(worker);

    // Create scheduler fiber
    worker->fiber_context.scheduler_fiber = CreateFiber(SMALL_FIBER_STACK_SIZE, scheduler_fiber_func, worker);

    // Initialize fiber context
    init_fiber_pools(&worker->fiber_context);
    worker->fiber_context.spin_count = 100;
    worker->fiber_context.consecutive_steals = 0;

    // Set thread affinity for better cache usage
    SetThreadAffinityMask(GetCurrentThread(), 1ULL << worker->worker_index);

    // Set high priority for game thread
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Start in scheduler fiber
    SwitchToFiber(worker->fiber_context.scheduler_fiber);

    // Cleanup (when should_quit is set)
    DeleteFiber(worker->fiber_context.scheduler_fiber);

    // Delete fiber pools
    for (u32 i = 0; i < SMALL_FIBER_POOL_SIZE; ++i)
    {
        if (worker->fiber_context.small_fibers.fibers[i])
        {
            DeleteFiber(worker->fiber_context.small_fibers.fibers[i]);
        }
    }
    for (u32 i = 0; i < LARGE_FIBER_POOL_SIZE; ++i)
    {
        if (worker->fiber_context.large_fibers.fibers[i])
        {
            DeleteFiber(worker->fiber_context.large_fibers.fibers[i]);
        }
    }

    ConvertFiberToThread();

    return 0;
}

// Main thread waiting
static void main_thread_wait(Job* job)
{
    while (job->unfinished_jobs > 0)
    {
        // Main thread - help with work by stealing
        static LONG robin = 0;
        u32 target = InterlockedIncrement(&robin) % g_job_system.worker_count;
        Job* stolen = deque_steal(g_job_system.workers[target].deque);
        if (stolen)
        {
            job_execute(stolen);
        }
        else
        {
            // Brief spin before trying again
            for (int i = 0; i < 10; ++i)
            {
                _mm_pause();
            }
        }
    }
}

// Initialize job system
bool job_system_init(u32 worker_count)
{
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);

    if (worker_count == 0 || worker_count > MAX_WORKER_THREADS)
    {
        worker_count = sys_info.dwNumberOfProcessors;
        if (worker_count > MAX_WORKER_THREADS)
        {
            worker_count = MAX_WORKER_THREADS;
        }
    }

    g_job_system.worker_count = worker_count;
    g_job_system.should_quit = 0;

    // Initialize job pool
    job_pool_init(&g_job_system.job_pool);

    // Initialize workers
    for (u32 i = 0; i < worker_count; ++i)
    {
        WorkerThread* worker = &g_job_system.workers[i];
        worker->worker_index = i;
        worker->jobs_executed = 0;
        worker->jobs_stolen = 0;

        // Allocate deque
        worker->deque = (WorkStealingQueue*) _aligned_malloc(sizeof(WorkStealingQueue), CACHE_LINE_SIZE);
        worker->deque->top = 0;
        worker->deque->bottom = 0;

        // Initialize priority queues
        for (int p = 0; p < JOB_PRIORITY_COUNT; ++p)
        {
            worker->priority_queues[p].head = 0;
            worker->priority_queues[p].tail = 0;
        }

        // Create worker thread
        worker->thread = CreateThread(NULL, 0, worker_thread_func, worker, 0, &worker->thread_id);
        SetThreadPriority(worker->thread, THREAD_PRIORITY_HIGHEST);
    }

    // Convert main thread to fiber for job waiting
    g_job_system.main_fiber = ConvertThreadToFiber(NULL);

    return true;
}

// Shutdown job system
void job_system_shutdown(void)
{
    g_job_system.should_quit = 1;

    // Submit dummy jobs to wake up spinning workers
    for (u32 i = 0; i < g_job_system.worker_count; ++i)
    {
        JobHandle wake_job = job_create(NULL, NULL);
        if (is_valid_handle(wake_job))
        {
            Job* job = get_job_from_handle(wake_job);
            if (job)
            {
                deque_push_bottom(g_job_system.workers[i].deque, job);
            }
        }
    }

    // Wait for workers to finish
    for (u32 i = 0; i < g_job_system.worker_count; ++i)
    {
        WaitForSingleObject(g_job_system.workers[i].thread, INFINITE);
        CloseHandle(g_job_system.workers[i].thread);
        _aligned_free(g_job_system.workers[i].deque);
    }

    ConvertFiberToThread();
}

// Create a job
JobHandle job_create(JobFunc func, void* data)
{
    JobHandle handle = job_alloc();
    if (!is_valid_handle(handle))
        return INVALID_JOB_HANDLE;

    Job* job = get_job_from_handle(handle);
    job->function = func;
    job->data = data;
    job->parent_index = MAX_JOB_COUNT;
    job->unfinished_jobs = 1;
    job->flags = JOB_FLAG_SMALL_STACK;

    return handle;
}

// Create a job with specific flags
JobHandle job_create_with_flags(JobFunc func, void* data, u8 flags)
{
    JobHandle handle = job_alloc();
    if (!is_valid_handle(handle))
        return INVALID_JOB_HANDLE;

    Job* job = get_job_from_handle(handle);
    job->function = func;
    job->data = data;
    job->parent_index = MAX_JOB_COUNT;
    job->unfinished_jobs = 1;
    job->flags = flags;

    return handle;
}

// Create a child job
JobHandle job_create_as_child(JobHandle parent_handle, JobFunc func, void* data)
{
    Job* parent = get_job_from_handle(parent_handle);
    if (!parent)
        return INVALID_JOB_HANDLE;

    InterlockedIncrement(&parent->unfinished_jobs);

    JobHandle handle = job_alloc();
    if (!is_valid_handle(handle))
    {
        InterlockedDecrement(&parent->unfinished_jobs);
        return INVALID_JOB_HANDLE;
    }

    u32 parent_index, parent_gen;
    unpack_job_handle(parent_handle, &parent_index, &parent_gen);

    Job* job = get_job_from_handle(handle);
    job->function = func;
    job->data = data;
    job->parent_index = parent_index;
    job->unfinished_jobs = 1;
    job->flags = parent->flags; // Inherit parent's flags by default

    return handle;
}

// Run a job
void job_run(JobHandle handle)
{
    Job* job = get_job_from_handle(handle);
    if (!job)
        return;

    WorkerThread* worker = get_current_worker();

    if (worker)
    {
        // Push to local deque
        if (!deque_push_bottom(worker->deque, job))
        {
            // Deque full, execute immediately
            job_execute(job);
        }
    }
    else
    {
        // Main thread, distribute to workers
        static LONG robin = 0;
        u32 target = InterlockedIncrement(&robin) % g_job_system.worker_count;
        if (!deque_push_bottom(g_job_system.workers[target].deque, job))
        {
            // Deque full, try next worker
            target = (target + 1) % g_job_system.worker_count;
            if (!deque_push_bottom(g_job_system.workers[target].deque, job))
            {
                // Still full, execute immediately
                job_execute(job);
            }
        }
    }
}

// Check if job is complete
bool job_is_complete(JobHandle handle)
{
    Job* job = get_job_from_handle(handle);
    if (!job)
        return true; // Invalid handles are "complete"

    return job->unfinished_jobs == 0;
}

// Wait for job completion
void job_wait(JobHandle handle)
{
    Job* job = get_job_from_handle(handle);
    if (!job)
        return;

    WorkerThread* worker = get_current_worker();

    if (worker)
    {
        // Worker thread - work while waiting
        while (job->unfinished_jobs > 0)
        {
            Job* other_job = worker_get_job(worker);
            if (other_job)
            {
                job_execute(other_job);
                InterlockedIncrement64((LONG64*) &worker->jobs_executed);
            }
            else
            {
                _mm_pause();
            }
        }
    }
    else
    {
        // Main thread
        main_thread_wait(job);
    }
}

// Batch operations
JobBatch job_batch_create(u32 capacity)
{
    JobBatch batch;
    batch.handles = (JobHandle*) malloc(capacity * sizeof(JobHandle));
    batch.capacity = capacity;
    batch.count = 0;
    return batch;
}

void job_batch_destroy(JobBatch* batch)
{
    free(batch->handles);
    batch->handles = NULL;
    batch->capacity = 0;
    batch->count = 0;
}

void job_batch_add(JobBatch* batch, JobHandle job)
{
    if (batch->count < batch->capacity)
    {
        batch->handles[batch->count++] = job;
    }
}

void job_batch_run(JobBatch* batch, JobPriority priority)
{
    WorkerThread* worker = get_current_worker();

    if (worker)
    {
        // Add to priority queue for better cache locality
        for (u32 i = 0; i < batch->count; ++i)
        {
            Job* job = get_job_from_handle(batch->handles[i]);
            if (!job)
                continue;

            if (!job_queue_push(&worker->priority_queues[priority], job))
            {
                // Queue full, fall back to deque
                if (!deque_push_bottom(worker->deque, job))
                {
                    // Deque also full, execute immediately
                    job_execute(job);
                }
            }
        }
    }
    else
    {
        // Distribute across workers
        for (u32 i = 0; i < batch->count; ++i)
        {
            Job* job = get_job_from_handle(batch->handles[i]);
            if (!job)
                continue;

            u32 target = i % g_job_system.worker_count;
            if (!job_queue_push(&g_job_system.workers[target].priority_queues[priority], job))
            {
                if (!deque_push_bottom(g_job_system.workers[target].deque, job))
                {
                    // Both full, execute immediately
                    job_execute(job);
                }
            }
        }
    }
}

void job_batch_wait(JobBatch* batch)
{
    for (u32 i = 0; i < batch->count; ++i)
    {
        job_wait(batch->handles[i]);
    }
}

// Parallel for implementation
typedef struct
{
    u32 start;
    u32 end;
    void (*func)(u32 start, u32 end, void* data);
    void* user_data;
} ParallelForData;

static void parallel_for_job(void* data)
{
    ParallelForData* pfd = (ParallelForData*) data;
    pfd->func(pfd->start, pfd->end, pfd->user_data);
}

JobHandle job_parallel_for(u32 count, u32 batch_size, void (*func)(u32 start, u32 end, void* data),
                           void* data)
{
    if (batch_size == 0)
    {
        batch_size = (count + g_job_system.worker_count - 1) / g_job_system.worker_count;
    }

    u32 batch_count = (count + batch_size - 1) / batch_size;

    // Create parent job for synchronization
    JobHandle parent = job_create(NULL, NULL);
    if (!is_valid_handle(parent))
        return INVALID_JOB_HANDLE;

    Job* parent_job = get_job_from_handle(parent);
    parent_job->unfinished_jobs = 0;

    // Create batch using dynamic allocation for safety
    ParallelForData* pfd = (ParallelForData*) malloc(batch_count * sizeof(ParallelForData));
    JobBatch batch = job_batch_create(batch_count);

    // Create batch jobs
    for (u32 i = 0; i < batch_count; ++i)
    {
        pfd[i].start = i * batch_size;
        pfd[i].end = min((i + 1) * batch_size, count);
        pfd[i].func = func;
        pfd[i].user_data = data;

        JobHandle child = job_create_as_child(parent, parallel_for_job, &pfd[i]);
        job_batch_add(&batch, child);
    }

    // Run all jobs
    job_batch_run(&batch, JOB_PRIORITY_NORMAL);

    // Clean up batch (but not pfd - jobs still need it)
    job_batch_destroy(&batch);

    // Store pfd pointer in parent job's data for cleanup
    parent_job->data = pfd;
    parent_job->function = (JobFunc) free; // Will free pfd when parent completes

    return parent;
}