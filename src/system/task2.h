#pragma once

// =================================================================================================
//
// A High-Performance, NUMA-Aware, Fiber-based, Work-Stealing Task System in C11
//
// Author: Gemini
// Date:   July 22, 2025
//
// Overview:
// This version integrates NUMA (Non-Uniform Memory Access) awareness for optimal
// performance on multi-socket or complex multi-core systems.
//
// Core Concepts:
// 1.  NUMA Affinity:
//     - The system automatically detects the hardware's NUMA topology.
//     - It creates one thread group per NUMA node and spawns one worker thread per
//       logical processor available to the process.
//     - Each worker thread is pinned to a specific logical core within its NUMA node
//       using an affinity mask. This prevents costly thread migration across nodes.
//
// 2.  NUMA-Aware Work-Stealing:
//     - When a thread runs out of work, it first attempts to steal from "sibling"
//       threads on the SAME NUMA node. This keeps memory access local and fast.
//     - Only if no local work is found will the thread attempt to steal from threads
//       on remote NUMA nodes. This ensures full CPU utilization while minimizing
//       cross-node memory traffic.
//
// 3.  Custom Fibers & Lock-Free Deque:
//     - The foundation remains a custom x64 fiber implementation for cooperative
//       multitasking and a lock-free deque for efficient, contention-free task sharing.
//
// Platform:
// - C11 standard for atomics (<stdatomic.h>).
// - Win32 API for threading and NUMA (<windows.h>).
// - x64 Inline Assembly (GCC/Clang syntax shown, notes for MSVC).
//
// =================================================================================================

#include <stdint.h>

#define DEFAULT_FIBER_STACK_SIZE (128 * 1024) // 128 KB stack for each fiber

typedef struct fiber_s fiber_t;
typedef struct task_system_s task_system_t;
typedef struct worker_thread_info_s worker_thread_info_t;

typedef struct { void* rsp; } fiber_context_t;
typedef enum { FIBER_STATE_READY, FIBER_STATE_RUNNING, FIBER_STATE_YIELDED, FIBER_STATE_DONE } fiber_state_e;
typedef void (*fiber_func_t)(void* args);

struct fiber_s {
	void* stack_base;
	void* stack_top;
	fiber_context_t ctx;
	fiber_func_t function;
	void* args;
	task_system_t* system;
	fiber_state_e state;
};

// A thread group now directly corresponds to a NUMA node.
typedef struct {
	const char* name;
	unsigned int numa_node_index;
	unsigned int thread_start_index;
	unsigned int thread_count;
} thread_group_t;

typedef struct { task_system_t* system; unsigned int thread_id; } thread_create_context_t;

// Symbols for the assembly functions.
// These are either defined as inline assembly (GCC/Clang) of binary blobs (MSVC).
#if __WIN64__ || _WIN64
extern const uint64_t fiber_switch_asm[];
extern const uint64_t fiber_init_asm[];
#else
// Avoid the MSVC hack unless necessary!
extern void* fiber_switch_asm(void** sp_from, void** sp_to, void* value);
extern fiber_t* fiber_init_asm(fiber_t* coro, void** sp_from, void* sp_to);
#endif

void fiber_start(fiber_t* fiber);
void fiber_switch(fiber_context_t* old_ctx, fiber_context_t* new_ctx);