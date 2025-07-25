#pragma once

#include "core.h"

// The maximum number of trace events that can be recorded per frame.
// If this is exceeded, subsequent events will be dropped.
#define MAX_PROFILER_EVENTS (1 << 12) // 4096

typedef struct ProfilerTraceEvent
{
	uint64_t start_tick;
	uint64_t end_tick;
	uint32_t worker_id;
	const char* name;
} ProfilerTraceEvent;

// Must be called once at application startup.
void profiler_init(void);

// Must be called once at application shutdown.
void profiler_shutdown(void);

// Call at the beginning of each frame to reset the event buffer.
void profiler_frame_start(void);

// Call at the end of each frame to print the timeline to the console.
void profiler_frame_end(void);

// Records a single job execution. This is thread-safe.
void profiler_record_job(uint64_t start, uint64_t end, uint32_t worker_id, const char* name);