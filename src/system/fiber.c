#include "core.h"
#include "fiber.h"
#include "deque.h"
#include "job.h"
#include "profiler.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>

WC_Worker g_workers[MAX_WORKERS];
uint32_t g_worker_count = 0;
long g_tls_worker_index;
static volatile long g_should_quit = 0;

__declspec(thread) WC_Worker* g_this_worker = NULL;

/* --- Forward Declarations --- */
static void worker_init(unsigned id);
static WC_Job* find_job_for_worker(WC_Worker* w);
static void __stdcall fiber_trampoline(void* fiber_param);
static DWORD WINAPI worker_thread_entry(LPVOID param);


/* --- Worker and Fiber Logic --- */

static DWORD WINAPI worker_thread_entry(LPVOID param)
{
    uint32_t worker_id = (uint32_t)(uintptr_t)param;
    worker_init(worker_id);

    while (!g_should_quit)
    {
        if (!fiber_execute_job()) {
            Sleep(0); 
        }
    }
    return 0;
}

static void __stdcall fiber_trampoline(void* fiber_param)
{
    WC_Fiber* self = (WC_Fiber*)fiber_param;

    while (!g_should_quit)
    {
        if (self->current_job)
        {
            WC_Job* job = self->current_job;
            uint64_t start_tick = __rdtsc();
            job->func(job->data);
            uint64_t end_tick = __rdtsc();
            profiler_record_job(start_tick, end_tick, g_this_worker->id, job->name);
            if (job->finish_callback)
            {
                job->finish_callback(job);
            }
        }
        
        self->current_job = NULL;
        self->available = 1;
        fiber_yield();
    }
}

/* --- Public API --- */

void fiber_pool_init(void)
{
    g_tls_worker_index = TlsAlloc();
    assert(g_tls_worker_index != TLS_OUT_OF_INDEXES);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_worker_count = min((unsigned)si.dwNumberOfProcessors, MAX_WORKERS);
    if (g_worker_count == 0) g_worker_count = 1;

    worker_init(0);

    for (uint32_t i = 1; i < g_worker_count; ++i)
    {
        g_workers[i].thread_handle = CreateThread(NULL, 0, worker_thread_entry, (LPVOID)(uintptr_t)i, 0, NULL);
        assert(g_workers[i].thread_handle != NULL);
    }
}

void fiber_pool_shutdown(void)
{
    g_should_quit = 1;
    
    if (g_worker_count > 1) {
        HANDLE handles[MAX_WORKERS-1];
        for (uint32_t i = 1; i < g_worker_count; ++i) {
            handles[i-1] = g_workers[i].thread_handle;
        }
        WaitForMultipleObjects(g_worker_count - 1, handles, TRUE, INFINITE);
    }

    for (uint32_t i = 0; i < g_worker_count; ++i)
    {
        WC_Worker* w = &g_workers[i];
        for (uint8_t j = 0; j < MAX_FIBERS_PER_WORKER; ++j) {
            if (w->fibers[j].fiber) DeleteFiber(w->fibers[j].fiber);
        }
        if (i > 0 && w->thread_handle) {
             CloseHandle(w->thread_handle);
        }
    }
    TlsFree(g_tls_worker_index);
}


bool fiber_execute_job(void)
{
    WC_Job* job = find_job_for_worker(g_this_worker);
    if (!job) return false;

    WC_Worker* w = g_this_worker;
    if (w->free_top > 0)
    {
        uint8_t fiber_idx = w->free[--w->free_top];
        WC_Fiber* fiber_obj = &w->fibers[fiber_idx];
        fiber_obj->available = 0;
        fiber_obj->current_job = job;
        
        SwitchToFiber(fiber_obj->fiber);
        
        if (fiber_obj->available)
        {
            w->free[w->free_top++] = fiber_idx;
        }
        return true;
    }

 uint64_t start_tick = __rdtsc();
    job->func(job->data);
 uint64_t end_tick = __rdtsc();
 profiler_record_job(start_tick, end_tick, g_this_worker->id, job->name);
    if(job->finish_callback) job->finish_callback(job);

    return true;
}

void fiber_yield(void)
{
    SwitchToFiber(g_this_worker->scheduler_fiber);
}

void fiber_switch_to_next(void)
{
    static volatile long next = 0;
    long idx = _InterlockedIncrement(&next) % g_worker_count;
    SwitchToFiber(g_workers[idx].scheduler_fiber);
}

/* --- Internal Implementations --- */

static void worker_init(unsigned id)
{
    WC_Worker* w = &g_workers[id];
    w->id = id;
    w->thread_handle = GetCurrentThread();

    w->scheduler_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    assert(w->scheduler_fiber);

    for (uint8_t i = 0; i < MAX_FIBERS_PER_WORKER; ++i)
    {
        w->fibers[i].fiber = CreateFiberEx(0, 16 * 1024, FIBER_FLAG_FLOAT_SWITCH, fiber_trampoline, &w->fibers[i]);
        assert(w->fibers[i].fiber);
        w->fibers[i].available = 1;
        w->fibers[i].current_job = NULL;
        w->free[i] = i;
    }
    w->free_top = MAX_FIBERS_PER_WORKER;
    deque_init(&w->deque);

    TlsSetValue(g_tls_worker_index, w);
    g_this_worker = w;
}

static WC_Job* find_job_for_worker(WC_Worker* w)
{
    WC_Job* job = deque_pop(&w->deque);
    if (job) return job;

    if (g_worker_count <= 1) return NULL;
    
    unsigned victim_idx = rand() % g_worker_count;
    if (&g_workers[victim_idx] == w) {
        victim_idx = (victim_idx + 1) % g_worker_count;
    }
    
    return deque_steal(&g_workers[victim_idx].deque);
}
