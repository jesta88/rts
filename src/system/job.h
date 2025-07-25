#pragma once

#include "core.h"

// Job priorities
typedef enum {
	JOB_PRIORITY_HIGH = 0,
	JOB_PRIORITY_NORMAL = 1,
	JOB_PRIORITY_LOW = 2,
	JOB_PRIORITY_COUNT = 3
} JobPriority;

// Job flags
typedef enum {
	JOB_FLAG_SMALL_STACK = 0,  // Use small fiber (default)
	JOB_FLAG_LARGE_STACK = 1   // Use large fiber for complex jobs
} JobFlags;

// Job function signature
typedef void (*JobFunc)(void* data);

/* --------------- one-time setup / teardown --------------- */
void job_init(void);
void job_shutdown(void);

/* --------------- producer API (thread-safe) -------------- */
WC_JobHandle job_schedule(const char* name, void (*func)(void*), void* data, WC_JobHandle after /*0 for none*/);

/* --------------- consumer API (job code) ----------------- */
void job_yield(void);	   /* cooperative yield           */
void job_wait(WC_JobHandle h); /* spin-yield until finished   */

/* --------------- per-frame flush (optional) -------------- */
void job_frame_end(void);