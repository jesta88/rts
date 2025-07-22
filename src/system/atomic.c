#include "atomic.h"
#include "memory.h"
#include "core.h"

//-------------------------------------------------------------------------------------------------
// Actual atomic implementations - hidden from header
//-------------------------------------------------------------------------------------------------

#if defined(_MSC_VER)

#define WIN32_LEAN_AND_MEAN
#include <intrin.h>
#include <windows.h>

// Memory barriers
#define WC_MEMORY_BARRIER_IMPL()     MemoryBarrier()
#define WC_READ_BARRIER_IMPL()       _ReadBarrier()
#define WC_WRITE_BARRIER_IMPL()      _WriteBarrier()
#define WC_COMPILER_BARRIER_IMPL()   _ReadWriteBarrier()
#define WC_CPU_PAUSE_IMPL()          YieldProcessor()

#elif defined(__GNUC__) || defined(__clang__)

#define WC_MEMORY_BARRIER_IMPL()     __sync_synchronize()
#define WC_READ_BARRIER_IMPL()       __asm__ __volatile__("" ::: "memory")
#define WC_WRITE_BARRIER_IMPL()      __asm__ __volatile__("" ::: "memory")
#define WC_COMPILER_BARRIER_IMPL()   __asm__ __volatile__("" ::: "memory")

#ifdef __x86_64__
#define WC_CPU_PAUSE_IMPL()          __builtin_ia32_pause()
#elif defined(__aarch64__)
#define WC_CPU_PAUSE_IMPL()          __asm__ __volatile__("yield")
#else
#define WC_CPU_PAUSE_IMPL()          do { } while(0)
#endif

#else
#error "Unsupported compiler"
#endif

//-------------------------------------------------------------------------------------------------
// Actual struct definitions - hidden from public interface
//-------------------------------------------------------------------------------------------------

struct WC_AtomicU64_Impl {
    _Alignas(8) volatile uint64_t value;
};

struct WC_AtomicPtr_Impl {
    _Alignas(8) volatile void* value;
};

struct WC_AtomicBool_Impl {
    _Alignas(8) volatile bool value;
};

struct WC_AtomicSize_Impl {
    _Alignas(8) volatile size_t value;
};

//-------------------------------------------------------------------------------------------------
// Memory barrier implementations
//-------------------------------------------------------------------------------------------------

void wc_atomic_fence_acquire(void) {
    WC_READ_BARRIER_IMPL();
}

void wc_atomic_fence_release(void) {
    WC_WRITE_BARRIER_IMPL();
}

void wc_atomic_fence_seq_cst(void) {
    WC_MEMORY_BARRIER_IMPL();
}

void wc_atomic_fence_compiler(void) {
    WC_COMPILER_BARRIER_IMPL();
}

void wc_cpu_pause(void) {
    WC_CPU_PAUSE_IMPL();
}

//-------------------------------------------------------------------------------------------------
// 64-bit atomic operations
//-------------------------------------------------------------------------------------------------

WC_AtomicU64 wc_atomic_u64_create(uint64_t initial_value) {
    struct WC_AtomicU64_Impl* atomic = wc_aligned_alloc(sizeof(struct WC_AtomicU64_Impl), 8);
    if (atomic) {
        atomic->value = initial_value;
    }
    return atomic;
}

void wc_atomic_u64_destroy(WC_AtomicU64 atomic) {
    if (atomic) {
        wc_aligned_free(atomic, 8);
    }
}

uint64_t wc_atomic_u64_load(WC_AtomicU64 atomic) {
	const struct WC_AtomicU64_Impl* impl = atomic;
	const uint64_t result = impl->value;
    WC_READ_BARRIER_IMPL();
    return result;
}

void wc_atomic_u64_store(WC_AtomicU64 atomic, uint64_t value) {
    struct WC_AtomicU64_Impl* impl = atomic;
    WC_WRITE_BARRIER_IMPL();
    impl->value = value;
}

uint64_t wc_atomic_u64_load_acquire(WC_AtomicU64 atomic) {
	const struct WC_AtomicU64_Impl* impl = atomic;
	const uint64_t result = impl->value;
    WC_READ_BARRIER_IMPL();
    return result;
}

void wc_atomic_u64_store_release(WC_AtomicU64 atomic, uint64_t value) {
    struct WC_AtomicU64_Impl* impl = atomic;
    WC_WRITE_BARRIER_IMPL();
    impl->value = value;
}

bool wc_atomic_u64_cas(WC_AtomicU64 atomic, uint64_t expected, uint64_t desired) {
    struct WC_AtomicU64_Impl* impl = atomic;

#if defined(_MSC_VER)
    return _InterlockedCompareExchange64((volatile LONGLONG*)&impl->value,
                                         (LONGLONG)desired, (LONGLONG)expected) == (LONGLONG)expected;
#else
    return __sync_bool_compare_and_swap(&impl->value, expected, desired);
#endif
}

uint64_t wc_atomic_u64_exchange(WC_AtomicU64 atomic, uint64_t value) {
    struct WC_AtomicU64_Impl* impl = atomic;

#if defined(_MSC_VER)
    return (uint64_t)_InterlockedExchange64((volatile LONGLONG*)&impl->value, (LONGLONG)value);
#else
    return __sync_lock_test_and_set(&impl->value, value);
#endif
}

uint64_t wc_atomic_u64_fetch_add(WC_AtomicU64 atomic, uint64_t value) {
    struct WC_AtomicU64_Impl* impl = atomic;

#if defined(_MSC_VER)
    return (uint64_t)_InterlockedExchangeAdd64((volatile LONGLONG*)&impl->value, (LONGLONG)value);
#else
    return __sync_fetch_and_add(&impl->value, value);
#endif
}

uint64_t wc_atomic_u64_fetch_sub(WC_AtomicU64 atomic, uint64_t value) {
    struct WC_AtomicU64_Impl* impl = atomic;

#if defined(_MSC_VER)
    return (uint64_t)_InterlockedExchangeAdd64((volatile LONGLONG*)&impl->value, -(LONGLONG)value);
#else
    return __sync_fetch_and_sub(&impl->value, value);
#endif
}

uint64_t wc_atomic_u64_increment(WC_AtomicU64 atomic) {
    struct WC_AtomicU64_Impl* impl = atomic;

#if defined(_MSC_VER)
    return (uint64_t)_InterlockedIncrement64((volatile LONGLONG*)&impl->value);
#else
    return __sync_add_and_fetch(&impl->value, 1);
#endif
}

uint64_t wc_atomic_u64_decrement(WC_AtomicU64 atomic) {
    struct WC_AtomicU64_Impl* impl = atomic;

#if defined(_MSC_VER)
    return (uint64_t)_InterlockedDecrement64((volatile LONGLONG*)&impl->value);
#else
    return __sync_sub_and_fetch(&impl->value, 1);
#endif
}

//-------------------------------------------------------------------------------------------------
// Pointer atomic operations
//-------------------------------------------------------------------------------------------------

WC_AtomicPtr wc_atomic_ptr_create(void* initial_value) {
    struct WC_AtomicPtr_Impl* atomic = wc_aligned_alloc(sizeof(struct WC_AtomicPtr_Impl), 8);
    if (atomic) {
        atomic->value = initial_value;
    }
    return atomic;
}

void wc_atomic_ptr_destroy(WC_AtomicPtr atomic) {
    if (atomic) {
        wc_aligned_free(atomic, 8);
    }
}

void* wc_atomic_ptr_load(WC_AtomicPtr atomic) {
	const struct WC_AtomicPtr_Impl* impl = atomic;
    void* result = (void*)impl->value;
    WC_READ_BARRIER_IMPL();
    return result;
}

void wc_atomic_ptr_store(WC_AtomicPtr atomic, void* value) {
    struct WC_AtomicPtr_Impl* impl = atomic;
    WC_WRITE_BARRIER_IMPL();
    impl->value = value;
}

void* wc_atomic_ptr_load_acquire(WC_AtomicPtr atomic) {
	const struct WC_AtomicPtr_Impl* impl = atomic;
    void* result = (void*)impl->value;
    WC_READ_BARRIER_IMPL();
    return result;
}

void wc_atomic_ptr_store_release(WC_AtomicPtr atomic, void* value) {
    struct WC_AtomicPtr_Impl* impl = atomic;
    WC_WRITE_BARRIER_IMPL();
    impl->value = value;
}

bool wc_atomic_ptr_cas(WC_AtomicPtr atomic, void* expected, void* desired) {
    struct WC_AtomicPtr_Impl* impl = atomic;

#if defined(_MSC_VER)
    return _InterlockedCompareExchange64((volatile LONGLONG*)&impl->value,
                                         (LONGLONG)desired, (LONGLONG)expected) == (LONGLONG)expected;
#else
    return __sync_bool_compare_and_swap(&impl->value, expected, desired);
#endif
}

void* wc_atomic_ptr_exchange(WC_AtomicPtr atomic, void* value) {
    struct WC_AtomicPtr_Impl* impl = atomic;

#if defined(_MSC_VER)
    return (void*)_InterlockedExchange64((volatile LONGLONG*)&impl->value, (LONGLONG)value);
#else
    return __sync_lock_test_and_set(&impl->value, value);
#endif
}

//-------------------------------------------------------------------------------------------------
// Boolean atomic operations
//-------------------------------------------------------------------------------------------------

WC_AtomicBool wc_atomic_bool_create(bool initial_value) {
    struct WC_AtomicBool_Impl* atomic = wc_aligned_alloc(sizeof(struct WC_AtomicBool_Impl), 8);
    if (atomic) {
        atomic->value = initial_value;
    }
    return atomic;
}

void wc_atomic_bool_destroy(WC_AtomicBool atomic) {
    if (atomic) {
        wc_aligned_free(atomic, 8);
    }
}

bool wc_atomic_bool_load(WC_AtomicBool atomic) {
	const struct WC_AtomicBool_Impl* impl = atomic;
    bool result = impl->value;
    WC_READ_BARRIER_IMPL();
    return result;
}

void wc_atomic_bool_store(WC_AtomicBool atomic, bool value) {
    struct WC_AtomicBool_Impl* impl = atomic;
    WC_WRITE_BARRIER_IMPL();
    impl->value = value;
}

bool wc_atomic_bool_cas(WC_AtomicBool atomic, bool expected, bool desired) {
    struct WC_AtomicBool_Impl* impl = atomic;

#if defined(_MSC_VER)
    return _InterlockedCompareExchange8((volatile char*)&impl->value,
                                        desired ? 1 : 0, expected ? 1 : 0) == (expected ? 1 : 0);
#else
    return __sync_bool_compare_and_swap(&impl->value, expected, desired);
#endif
}

bool wc_atomic_bool_exchange(WC_AtomicBool atomic, bool value) {
    struct WC_AtomicBool_Impl* impl = atomic;

#if defined(_MSC_VER)
    return _InterlockedExchange8((volatile char*)&impl->value, value ? 1 : 0) != 0;
#else
    return __sync_lock_test_and_set(&impl->value, value);
#endif
}

//-------------------------------------------------------------------------------------------------
// Size atomic operations (aliases to 64-bit on x64)
//-------------------------------------------------------------------------------------------------

WC_AtomicSize wc_atomic_size_create(size_t initial_value) {
    return (WC_AtomicSize)wc_atomic_u64_create(initial_value);
}

void wc_atomic_size_destroy(WC_AtomicSize atomic) {
    wc_atomic_u64_destroy((WC_AtomicU64)atomic);
}

size_t wc_atomic_size_load(WC_AtomicSize atomic) {
    return wc_atomic_u64_load((WC_AtomicU64)atomic);
}

void wc_atomic_size_store(WC_AtomicSize atomic, size_t value) {
    wc_atomic_u64_store((WC_AtomicU64)atomic, value);
}

size_t wc_atomic_size_load_acquire(WC_AtomicSize atomic) {
    return wc_atomic_u64_load_acquire((WC_AtomicU64)atomic);
}

void wc_atomic_size_store_release(WC_AtomicSize atomic, size_t value) {
    wc_atomic_u64_store_release((WC_AtomicU64)atomic, value);
}

bool wc_atomic_size_cas(WC_AtomicSize atomic, size_t expected, size_t desired) {
    return wc_atomic_u64_cas((WC_AtomicU64)atomic, expected, desired);
}

size_t wc_atomic_size_exchange(WC_AtomicSize atomic, size_t value) {
    return wc_atomic_u64_exchange((WC_AtomicU64)atomic, value);
}

size_t wc_atomic_size_fetch_add(WC_AtomicSize atomic, size_t value) {
    return wc_atomic_u64_fetch_add((WC_AtomicU64)atomic, value);
}

size_t wc_atomic_size_fetch_sub(WC_AtomicSize atomic, size_t value) {
    return wc_atomic_u64_fetch_sub((WC_AtomicU64)atomic, value);
}

size_t wc_atomic_size_increment(WC_AtomicSize atomic) {
    return wc_atomic_u64_increment((WC_AtomicU64)atomic);
}

size_t wc_atomic_size_decrement(WC_AtomicSize atomic) {
    return wc_atomic_u64_decrement((WC_AtomicU64)atomic);
}