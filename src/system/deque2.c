#include "deque2.h"
#include "memory.h"

static void deque_init(lock_free_deque_t* deque)
{
	deque->tasks = (task_t*)wc_malloc(sizeof(task_t) * DEQUE_CAPACITY);
	deque->top = wc_atomic_size_create(0);
	deque->bottom = wc_atomic_size_create(0);
}
static void deque_destroy(lock_free_deque_t* deque)
{
	if (deque->tasks)
		wc_free(deque->tasks);
}

static bool deque_push(lock_free_deque_t* deque, task_t task)
{
	size_t bottom = wc_atomic_size_load(deque->bottom);
	size_t top = wc_atomic_size_load(deque->top);
	if (bottom - top >= DEQUE_CAPACITY)
		return false;
	deque->tasks[bottom & (DEQUE_CAPACITY - 1)] = task;
	wc_atomic_size_store(deque->bottom, bottom + 1);
	return true;
}

static bool deque_pop(lock_free_deque_t* deque, task_t* task)
{
	size_t bottom = wc_atomic_size_load(deque->bottom) - 1;
	wc_atomic_size_store(deque->bottom, bottom);
	size_t top = wc_atomic_size_load(deque->top);
	if (top > bottom)
	{
		wc_atomic_size_store(deque->bottom, top);
		return false;
	}
	*task = deque->tasks[bottom & (DEQUE_CAPACITY - 1)];
	if (top == bottom)
	{
		if (wc_atomic_size_cas(deque->top, top, top + 1))
		{
			wc_atomic_size_store(deque->bottom, top + 1);
			return true;
		}
		else
		{
			wc_atomic_size_store(deque->bottom, top + 1);
			return false;
		}
	}
	return true;
}

static bool deque_steal(lock_free_deque_t* deque, task_t* task)
{
	size_t top = wc_atomic_size_load(deque->top);
	size_t bottom = wc_atomic_size_load(deque->bottom);
	if (top >= bottom)
		return false;
	*task = deque->tasks[top & (DEQUE_CAPACITY - 1)];
	if (wc_atomic_size_cas(deque->top, top, top + 1))
	{
		return true;
	}
	return false;
}
