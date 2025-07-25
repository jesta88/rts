#pragma once

// Define a portable alignment macro that works on MSVC, GCC, and Clang.
#if defined(_MSC_VER)
    #define WC_ALIGN(N) __declspec(align(N))
#else
    #define WC_ALIGN(N) __attribute__((aligned(N)))
#endif

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#define WC_KB 1024
#define WC_MB (WC_KB * WC_KB)
#define WC_GB (WC_MB * WC_MB)
#define WC_STRINGIFY_INTERNAL(...) #__VA_ARGS__
#define WC_STRINGIFY(...) WC_STRINGIFY_INTERNAL(__VA_ARGS__)
#define WC_ALIGN_TRUNCATE(v, n) ((v) & ~((n) - 1))
#define WC_ALIGN_FORWARD(v, n) WC_ALIGN_TRUNCATE((v) + (n) - 1, (n))

#define CACHE_LINE 64
#define QUEUE_MASK 255u

enum
{
	WC_MAX_CHILDREN = 6
};

// Forward-declare the job struct for the function pointer typedef.
typedef struct WC_Job WC_Job;

typedef void (*WC_JobFn)(void*);
typedef void (*WC_JobFinishFn)(WC_Job*);

struct WC_Job
{
	WC_JobFn func;
	void* data;
	WC_JobFinishFn finish_callback;
	const char* name;

	WC_ALIGN(CACHE_LINE) volatile long depLeft;
	uint32_t childIdx[WC_MAX_CHILDREN];
	volatile uint32_t childCnt;
	// Correct padding to ensure the struct is a multiple of the cache line size.
	uint32_t pad[(CACHE_LINE - (sizeof(volatile long) + sizeof(uint32_t) * WC_MAX_CHILDREN + sizeof(volatile uint32_t))) / sizeof(uint32_t)];
};

typedef struct WC_JobHandle
{
	uint32_t idx;
	uint32_t gen;
} WC_JobHandle;

typedef struct WC_Deque
{
	WC_ALIGN(CACHE_LINE) volatile uint32_t top;
	WC_ALIGN(CACHE_LINE) volatile uint32_t bottom;
	WC_Job* ring[QUEUE_MASK + 1];
} WC_Deque;

typedef struct WC_Fiber
{
	WC_ALIGN(CACHE_LINE) void* fiber;
	WC_Job* current_job;
	uint8_t available;
	// Correct padding.
	uint8_t pad[CACHE_LINE - sizeof(void*) - sizeof(WC_Job*) - sizeof(uint8_t)];
} WC_Fiber;

#define MAX_FIBERS_PER_WORKER 64

typedef struct WC_Worker
{
	WC_ALIGN(CACHE_LINE) WC_Deque deque;
	WC_Fiber fibers[MAX_FIBERS_PER_WORKER];
	uint8_t free_top;
	uint8_t free[MAX_FIBERS_PER_WORKER];
    
    WC_ALIGN(CACHE_LINE) void* scheduler_fiber;
    void* thread_handle;
    uint32_t id;
} WC_Worker;