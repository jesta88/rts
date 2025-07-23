#include "task2.h"

#include <mimalloc.h>
#define WIN32_LEAN_AND_MEAN
#include "SDL3/SDL_log.h"
#include "deque2.h"

#include <stdint.h>
#include <windows.h>

// Use thread-local storage to track the current running fiber and worker context.
__declspec(thread) fiber_t* g_current_fiber;
__declspec(thread) worker_thread_info_t* g_current_worker;

// Per-thread data, now includes NUMA info
struct worker_thread_info_s {
	HANDLE handle;
	fiber_context_t ctx; // The main context for this worker thread's scheduler loop
	task_system_t* system;
	unsigned int thread_id_in_system;
	unsigned int numa_node_index;
};

#if __GNUC__
__asm__(
	".global fiber_switch\n"
	"fiber_switch:\n"
	"push %rbx; push %rbp; push %rdi; push %rsi; push %r12; push %r13; push %r14; push %r15\n"
	"movq %rsp, (%rcx)\n"
	"movq (%rdx), %rsp\n"
	"pop %r15; pop %r14; pop %r13; pop %r12; pop %rsi; pop %rdi; pop %rbp; pop %rbx\n"
	"ret\n"
);
#elif _MSC_VER
#pragma section(".text")

__declspec(allocate(".text"))
	const uint64_t fiber_init_stack[] = {
	0x5541544157565355, 0x2534ff6557415641,
	0x2534ff6500000008, 0x2534ff6500000010,
	0xa0ec814800001478, 0x9024b4290f000000,
	0x8024bc290f000000, 0x2444290f44000000,
	0x4460244c290f4470, 0x290f44502454290f,
	0x2464290f4440245c, 0x4420246c290f4430,
	0x290f44102474290f, 0xe08349228948243c,
	0x0c894865c4894cf0, 0x8948650000147825,
	0x4c6500000010250c, 0x6a00000008250489,
	0xb8489020ec834800, (uint64_t)fiber_start,
	0x909090909090e0ff, 0x9090909090909090,
};

// Assembled and dumped from win64-swap.S
__declspec(allocate(".text"))
const uint64_t fiber_switch_asm[] = {
	0x5541544157565355, 0x2534ff6557415641,
	0x2534ff6500000008, 0x2534ff6500000010,
	0xa0ec814800001478, 0x9024b4290f000000,
	0x8024bc290f000000, 0x2444290f44000000,
	0x4460244c290f4470, 0x290f44502454290f,
	0x2464290f4440245c, 0x4420246c290f4430,
	0x290f44102474290f, 0x228b48218948243c,
	0x0000009024b4280f, 0x0000008024bc280f,
	0x0f44702444280f44, 0x54280f4460244c28,
	0x40245c280f445024, 0x0f44302464280f44,
	0x74280f4420246c28, 0x48243c280f441024,
	0x8f65000000a0c481, 0x8f65000014782504,
	0x8f65000000102504, 0x5f41000000082504,
	0x5e5f5c415d415e41, 0x9090c3c0894c5d5b,
};
#endif

static void fiber_entry_point(void) {
    // Call the user-provided function
    g_current_fiber->function(g_current_fiber->args);
    // When the function returns, the fiber is done.
    g_current_fiber->state = FIBER_STATE_DONE;
    // Switch back to the worker's main context.
    fiber_switch(&g_current_fiber->ctx, &g_current_worker->ctx);
}

fiber_t* fiber_create(size_t stack_size, fiber_func_t function, void* args, task_system_t* system) {
    fiber_t* fiber = mi_malloc_aligned(sizeof(fiber_t), 16);
    if (!fiber) return NULL;

    stack_size = stack_size > 0 ? stack_size : DEFAULT_FIBER_STACK_SIZE;
    fiber->stack_base = mi_malloc_aligned(stack_size, 16);
    if (!fiber->stack_base) { mi_free(fiber); return NULL; }

    fiber->stack_top = (char*)fiber->stack_base + stack_size;
    fiber->function = function;
    fiber->args = args;
    fiber->system = system;
    fiber->state = FIBER_STATE_READY;

    void** stack_ptr = (void**)fiber->stack_top;
    *--stack_ptr = (void*)fiber_entry_point; // Set the return address for fiber_switch
    fiber->ctx.rsp = (char*)stack_ptr - (8 * 8); // Make space for saved registers

    return fiber;
}

void fiber_destroy(fiber_t* fiber) {
    if (!fiber) return;
    mi_free(fiber->stack_base);
    mi_free(fiber);
}

void fiber_yield() {
    g_current_fiber->state = FIBER_STATE_YIELDED;
    fiber_switch(&g_current_fiber->ctx, &g_current_worker->ctx);
}

struct task_system_s {
	worker_thread_info_t threads[MAX_THREADS];
	lock_free_deque_t deques[MAX_THREADS];
	thread_group_t groups[MAX_NUMA_NODES];

	unsigned int num_threads;
	unsigned int num_numa_nodes;

	WC_AtomicBool is_running;
	WC_AtomicSize tasks_in_flight;
};

// =================================================================================================
// Worker Thread Logic (NUMA-Aware Stealing)
// =================================================================================================

static DWORD WINAPI worker_thread_func(LPVOID arg) {
    thread_create_context_t* create_ctx = (thread_create_context_t*)arg;
    g_current_worker = &create_ctx->system->threads[create_ctx->thread_id];
    lock_free_deque_t* my_deque = &g_current_worker->system->deques[g_current_worker->thread_id_in_system];
    thread_group_t* my_group = &g_current_worker->system->groups[g_current_worker->numa_node_index];
    mi_free(create_ctx);

    task_t task_fiber;

    while (wc_atomic_bool_load(g_current_worker->system->is_running)) {
        // 1. Try to get work from our own deque (LIFO).
        if (deque_pop(my_deque, &task_fiber)) {
            g_current_fiber = task_fiber;
            g_current_fiber->state = FIBER_STATE_RUNNING;
            fiber_switch(&g_current_worker->ctx, &g_current_fiber->ctx);

            if (g_current_fiber->state == FIBER_STATE_DONE) {
                fiber_destroy(g_current_fiber);
                wc_atomic_size_fetch_sub(g_current_worker->system->tasks_in_flight, 1);
            } else if (g_current_fiber->state == FIBER_STATE_YIELDED) {
                deque_push(my_deque, g_current_fiber);
            }
            g_current_fiber = NULL;
            continue;
        }

        bool found_work = false;

        // 2. Our deque is empty. Try to steal from siblings on the SAME NUMA node.
        for (unsigned int i = 0; i < my_group->thread_count; ++i) {
            unsigned int victim_offset = SDL_rand(UINT32_MAX) % my_group->thread_count;
            unsigned int victim_id = my_group->thread_start_index + victim_offset;
            if (victim_id == g_current_worker->thread_id_in_system) continue;

            if (deque_steal(&g_current_worker->system->deques[victim_id], &task_fiber)) {
                found_work = true;
                break;
            }
        }

        // 3. Still no work. Expand search and try to steal from ANY other thread (remote NUMA node).
        if (!found_work) {
            for (unsigned int i = 0; i < g_current_worker->system->num_threads; ++i) {
                unsigned int victim_id = SDL_rand(UINT32_MAX) % g_current_worker->system->num_threads;
                if (victim_id == g_current_worker->thread_id_in_system) continue;

                if (deque_steal(&g_current_worker->system->deques[victim_id], &task_fiber)) {
                    found_work = true;
                    break;
                }
            }
        }

        if (found_work) {
            g_current_fiber = task_fiber;
            g_current_fiber->state = FIBER_STATE_RUNNING;
            fiber_switch(&g_current_worker->ctx, &g_current_fiber->ctx);

            if (g_current_fiber->state == FIBER_STATE_DONE) {
                fiber_destroy(g_current_fiber);
                wc_atomic_size_fetch_sub(g_current_worker->system->tasks_in_flight, 1);
            } else if (g_current_fiber->state == FIBER_STATE_YIELDED) {
                // If we stole it, we own it now. Push to our local deque.
                deque_push(my_deque, g_current_fiber);
            }
            g_current_fiber = NULL;
            continue;
        }

        // 4. No work found anywhere. Back off.
        if (wc_atomic_size_load(g_current_worker->system->tasks_in_flight) == 0) {
            Sleep(1);
        } else {
            SwitchToThread(); // Yield time slice
        }
    }
    return 0;
}

task_system_t* task_system_create() {
    task_system_t* system = (task_system_t*)calloc(1, sizeof(task_system_t));
    if (!system) return NULL;

    system->is_running = wc_atomic_bool_create(true);
    system->tasks_in_flight = wc_atomic_size_create(0);

    ULONG highest_node_number = 0;
    if (!GetNumaHighestNodeNumber(&highest_node_number)) {
        // System is not NUMA-aware, treat as a single node.
        highest_node_number = 0;
    }
    system->num_numa_nodes = highest_node_number + 1;
    SDL_Log("Task System: Detected %u NUMA node(s).\n", system->num_numa_nodes);

    unsigned int thread_count = 0;
    for (ULONG node_idx = 0; node_idx <= highest_node_number; ++node_idx) {
        GROUP_AFFINITY affinity;
        if (!GetNumaNodeProcessorMaskEx((USHORT)node_idx, &affinity)) {
            // Fallback for non-NUMA or older systems
            GetProcessAffinityMask(GetCurrentProcess(), &affinity.Mask, &(ULONG_PTR){0});
            affinity.Group = 0;
        }

        system->groups[node_idx].numa_node_index = node_idx;
        system->groups[node_idx].thread_start_index = thread_count;
        system->groups[node_idx].thread_count = 0;

        SDL_Log("  - Node %lu, Affinity Mask: 0x%llX\n", node_idx, affinity.Mask);

        for (int i = 0; i < sizeof(affinity.Mask) * 8; ++i) {
            ULONG_PTR mask = (ULONG_PTR)1 << i;
            if (affinity.Mask & mask) {
                if (thread_count >= MAX_THREADS) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Warning: Exceeded MAX_THREADS, some cores will not be used.\n");
                    break;
                }

                unsigned int current_thread_id = thread_count;
                system->threads[current_thread_id].thread_id_in_system = current_thread_id;
                system->threads[current_thread_id].numa_node_index = node_idx;
                system->threads[current_thread_id].system = system;
                deque_init(&system->deques[current_thread_id]);

                thread_create_context_t* create_ctx = (thread_create_context_t*)malloc(sizeof(thread_create_context_t));
                create_ctx->system = system;
                create_ctx->thread_id = current_thread_id;

                system->threads[current_thread_id].handle = CreateThread(NULL, 0, worker_thread_func, create_ctx, CREATE_SUSPENDED, NULL);
                SetThreadAffinityMask(system->threads[current_thread_id].handle, mask);
                ResumeThread(system->threads[current_thread_id].handle);

                system->groups[node_idx].thread_count++;
                thread_count++;
            }
        }
    }
    system->num_threads = thread_count;
    SDL_Log("Task System: Initialized with %u total threads across %u NUMA nodes.\n", system->num_threads, system->num_numa_nodes);
    return system;
}

void task_system_destroy(task_system_t* system) {
    if (!system) return;
    SDL_Log("Task System: Shutting down...\n");
    wc_atomic_bool_store(system->is_running, false);

    HANDLE handles[MAX_THREADS];
    for (unsigned int i = 0; i < system->num_threads; ++i) {
        handles[i] = system->threads[i].handle;
    }
    WaitForMultipleObjects(system->num_threads, handles, TRUE, INFINITE);

    for (unsigned int i = 0; i < system->num_threads; ++i) {
        CloseHandle(system->threads[i].handle);
        deque_destroy(&system->deques[i]);
    }
    SDL_Log("Task System: Shutdown complete.\n");
    mi_free(system);
}

void task_system_submit(task_system_t* system, fiber_t* fiber, unsigned int numa_node_id) {
    if (numa_node_id >= system->num_numa_nodes) {
        numa_node_id = SDL_rand(UINT32_MAX) % system->num_numa_nodes; // Fallback to random node
    }

    wc_atomic_size_fetch_add(system->tasks_in_flight, 1);
    thread_group_t* group = &system->groups[numa_node_id];

    // Submit to a random thread within the specified NUMA node group
    unsigned int start_idx = group->thread_start_index;
    unsigned int thread_idx = start_idx + (SDL_rand(UINT32_MAX) % group->thread_count);

    while (!deque_push(&system->deques[thread_idx], fiber)) {
        thread_idx = start_idx + ((thread_idx - start_idx + 1) % group->thread_count);
    }
}

void task_system_wait(task_system_t* system) {
    while (wc_atomic_size_load(system->tasks_in_flight) > 0) {
        Sleep(1); // Simple wait. A real wait should also help execute tasks.
    }
}