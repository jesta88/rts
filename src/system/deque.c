#include "core.h"
#include "deque.h"

#include <intrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static inline uint32_t idx(uint32_t i)
{
	return i & QUEUE_MASK;
}

void deque_init(WC_Deque* deque)
{
	deque->top = 0;
	deque->bottom = 0;
}

void deque_push(WC_Deque* deque, WC_Job* job)
{
	long b = deque->bottom;
	deque->ring[idx(b)] = job;
	_WriteBarrier();
	deque->bottom = b + 1;
}

WC_Job* deque_pop(WC_Deque* deque)
{
	long b = deque->bottom;
	if (b == 0) { // Should not happen if we always check for jobs first, but as a safeguard.
		return NULL;
	}
	b--;
	deque->bottom = b;
	
	// This barrier is critical. It ensures the write to 'bottom' is globally visible
	// before we read 'top'. This prevents a race where a thief might see the old 'bottom'
	// but the new 'top', leading it to believe the queue is smaller than it is.
	MemoryBarrier();

	long t = deque->top;
	if (t <= b)
	{
		// Queue is not empty.
		WC_Job* job = deque->ring[idx(b)];
		if (t == b)
		{
			// Last item in queue, race against thieves.
			if (_InterlockedCompareExchange((volatile long*)&deque->top, t + 1, t) != t)
			{
				// Thief got it.
				job = NULL;
			}
		}
		// If t < b, there are multiple items, pop is safe as only owner touches bottom.
		return job;
	}
	else
	{
		// Queue was empty after all. Restore bottom.
		deque->bottom = t;
		return NULL;
	}
}

WC_Job* deque_steal(WC_Deque* deque)
{
	long t = deque->top;
	_ReadBarrier();
	long b = deque->bottom;

	if (t < b)
	{
		WC_Job* job = deque->ring[idx(t)];
		if (_InterlockedCompareExchange((volatile long*)&deque->top, t + 1, t) != t)
		{
			return NULL;
		}
		return job;
	}
	
	return NULL;
}
