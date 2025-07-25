#include "profiler.h"
#include "core.h"
#include "fiber.h" // For g_worker_count
#include <stdio.h>
#include <intrin.h>

#include <SDL3/SDL_log.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static ProfilerTraceEvent g_profiler_events[MAX_PROFILER_EVENTS];
static volatile long g_profiler_event_count;
static uint64_t g_profiler_frame_start_tick;
static uint64_t g_ticks_per_second;

void profiler_init(void)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_ticks_per_second = frequency.QuadPart;
    g_profiler_event_count = 0;
}

void profiler_shutdown(void)
{
    // Nothing to do
}

void profiler_frame_start(void)
{
    g_profiler_event_count = 0;
    g_profiler_frame_start_tick = __rdtsc();
}

void profiler_record_job(uint64_t start, uint64_t end, uint32_t worker_id, const char* name)
{
    long idx = _InterlockedIncrement(&g_profiler_event_count) - 1;
    if (idx >= MAX_PROFILER_EVENTS) {
        return;
    }
    g_profiler_events[idx].start_tick = start;
    g_profiler_events[idx].end_tick = end;
    g_profiler_events[idx].worker_id = worker_id;
    g_profiler_events[idx].name = name;
}

void profiler_frame_end(void)
{
    uint64_t frame_end_tick = __rdtsc();
    double frame_duration_ms = ((frame_end_tick - g_profiler_frame_start_tick) * 1000.0) / g_ticks_per_second;
    
    SDL_Log("\n--- FRAME TIMELINE (%.2f ms) ---\n", frame_duration_ms);

    for (uint32_t i = 0; i < g_worker_count; ++i)
    {
        SDL_Log("Worker %-2u: ", i);
        uint64_t last_end_tick = g_profiler_frame_start_tick;

        for (long j = 0; j < g_profiler_event_count; ++j)
        {
            if (g_profiler_events[j].worker_id != i) continue;

            // This is a simplified visualization. A real implementation would sort events by start time.
            ProfilerTraceEvent* e = &g_profiler_events[j];

            double idle_ms = ((e->start_tick - last_end_tick) * 1000.0) / g_ticks_per_second;
            double job_ms = ((e->end_tick - e->start_tick) * 1000.0) / g_ticks_per_second;
            
            if (idle_ms > 0.01) SDL_Log("[ idle: %.2f ms ]", idle_ms);
            SDL_Log("[ %s: %.2f ms ]", e->name, job_ms);

            last_end_tick = e->end_tick;
        }
        SDL_Log("\n");
    }
    SDL_Log("---------------------------------\n");
}