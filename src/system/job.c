#include "core.h"
#include "job.h"
#include "fiber.h"
#include "deque.h"

#include <intrin.h>
#include <assert.h>

enum
{
	MAX_JOBS = 1u << 16
};

typedef struct JobSlot
{
	WC_ALIGN(CACHE_LINE) WC_Job job;
	volatile long generation;
} JobSlot;

static JobSlot g_jobs[MAX_JOBS];
static volatile long g_job_next_free;

WC_JobHandle g_job_none = {0};

/* --- Internal --- */

static inline WC_JobHandle make_handle(uint32_t idx, long gen)
{
	return (WC_JobHandle){idx, (uint32_t)gen};
}

void job_finish_callback(WC_Job* job)
{
	for (uint32_t i = 0; i < job->childCnt; ++i)
	{
		uint32_t child_idx = job->childIdx[i];
		if (child_idx == 0) continue; 

		JobSlot* child_slot = &g_jobs[child_idx];
		if (_InterlockedDecrement(&child_slot->job.depLeft) == 0)
		{
			deque_push(&g_this_worker->deque, &child_slot->job);
		}
	}
}


/* --- Public API --- */

WC_JobHandle job_schedule(const char* name, void (*func)(void*), void* data, WC_JobHandle after)
{
	long idx = _InterlockedIncrement(&g_job_next_free) - 1;
	idx = (idx % (MAX_JOBS - 1)) + 1; // Keep 0 as invalid
	JobSlot* slot = &g_jobs[idx];

	long current_gen = _InterlockedExchangeAdd(&slot->generation, 1) + 1;

	slot->job.name = name;
	slot->job.func = func;
	slot->job.data = data;
	slot->job.depLeft = 1; 
	slot->job.childCnt = 0;
	slot->job.finish_callback = job_finish_callback;

	if (after.idx != 0 && g_jobs[after.idx].generation == after.gen)
	{
		JobSlot* parent = &g_jobs[after.idx];
		long child_count = _InterlockedIncrement((volatile long*)&parent->job.childCnt) - 1;
		
		if (child_count < WC_MAX_CHILDREN)
		{
			parent->job.childIdx[child_count] = (uint32_t)idx;
			_InterlockedIncrement(&slot->job.depLeft);
		}
		else
		{
			_InterlockedDecrement((volatile long*)&parent->job.childCnt);
		}
	}
	
	if (_InterlockedDecrement(&slot->job.depLeft) == 0)
	{
		deque_push(&g_this_worker->deque, &slot->job);
	}

	return make_handle((uint32_t)idx, current_gen);
}

void job_wait(WC_JobHandle h)
{
    if (h.idx == 0) return;
	JobSlot* slot = &g_jobs[h.idx];
	if (slot->generation != h.gen) return;

	// We use a CAS as an atomic read with acquire semantics to ensure that
	// we see all memory writes from the thread that decremented depLeft.
	while (_InterlockedCompareExchange(&slot->job.depLeft, 0, 0) > 0)
	{
		if (!fiber_execute_job()) {
			job_yield();
		}
	}
}

void job_yield(void)
{
	fiber_yield();
}

void job_init(void)
{
	fiber_pool_init();

	g_job_next_free = 1;
	for (int i = 0; i < MAX_JOBS; ++i) {
		g_jobs[i].generation = 0;
		g_jobs[i].job.depLeft = 0;
	}
}

void job_shutdown(void)
{
	fiber_pool_shutdown();
}

void job_frame_end(void)
{
	g_job_next_free = 1;
}
