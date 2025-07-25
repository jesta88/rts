#pragma once

#include "core.h"
#include <stdbool.h> // For bool type

#define MAX_WORKERS 32

extern WC_Worker g_workers[MAX_WORKERS];
extern uint32_t g_worker_count;
extern long g_tls_worker_index;

extern __declspec(thread) WC_Worker* g_this_worker;

void fiber_pool_init(void);
void fiber_pool_shutdown(void);

void fiber_yield(void);

// Finds and executes one job. Returns true if a job was run.
bool fiber_execute_job(void);

// Switches to the scheduler of the next available worker.
void fiber_switch_to_next(void);
