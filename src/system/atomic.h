#pragma once

#include <stdint.h>
#include <stdbool.h>

//-------------------------------------------------------------------------------------------------
// Opaque atomic types
//-------------------------------------------------------------------------------------------------

typedef struct WC_AtomicU64_Impl* WC_AtomicU64;
typedef struct WC_AtomicPtr_Impl* WC_AtomicPtr;
typedef struct WC_AtomicBool_Impl* WC_AtomicBool;
typedef struct WC_AtomicSize_Impl* WC_AtomicSize;

//-------------------------------------------------------------------------------------------------
// Atomic operations
//-------------------------------------------------------------------------------------------------

// Memory barriers
void wc_atomic_fence_acquire(void);
void wc_atomic_fence_release(void);
void wc_atomic_fence_seq_cst(void);
void wc_atomic_fence_compiler(void);
void wc_cpu_pause(void);

// 64-bit operations
WC_AtomicU64 wc_atomic_u64_create(uint64_t initial_value);
void wc_atomic_u64_destroy(WC_AtomicU64 atomic);
uint64_t wc_atomic_u64_load(WC_AtomicU64 atomic);
void wc_atomic_u64_store(WC_AtomicU64 atomic, uint64_t value);
uint64_t wc_atomic_u64_load_acquire(WC_AtomicU64 atomic);
void wc_atomic_u64_store_release(WC_AtomicU64 atomic, uint64_t value);
bool wc_atomic_u64_cas(WC_AtomicU64 atomic, uint64_t expected, uint64_t desired);
uint64_t wc_atomic_u64_exchange(WC_AtomicU64 atomic, uint64_t value);
uint64_t wc_atomic_u64_fetch_add(WC_AtomicU64 atomic, uint64_t value);
uint64_t wc_atomic_u64_fetch_sub(WC_AtomicU64 atomic, uint64_t value);
uint64_t wc_atomic_u64_increment(WC_AtomicU64 atomic);
uint64_t wc_atomic_u64_decrement(WC_AtomicU64 atomic);

// Pointer operations
WC_AtomicPtr wc_atomic_ptr_create(void* initial_value);
void wc_atomic_ptr_destroy(WC_AtomicPtr atomic);
void* wc_atomic_ptr_load(WC_AtomicPtr atomic);
void wc_atomic_ptr_store(WC_AtomicPtr atomic, void* value);
void* wc_atomic_ptr_load_acquire(WC_AtomicPtr atomic);
void wc_atomic_ptr_store_release(WC_AtomicPtr atomic, void* value);
bool wc_atomic_ptr_cas(WC_AtomicPtr atomic, void* expected, void* desired);
void* wc_atomic_ptr_exchange(WC_AtomicPtr atomic, void* value);

// Boolean operations
WC_AtomicBool wc_atomic_bool_create(bool initial_value);
void wc_atomic_bool_destroy(WC_AtomicBool atomic);
bool wc_atomic_bool_load(WC_AtomicBool atomic);
void wc_atomic_bool_store(WC_AtomicBool atomic, bool value);
bool wc_atomic_bool_cas(WC_AtomicBool atomic, bool expected, bool desired);
bool wc_atomic_bool_exchange(WC_AtomicBool atomic, bool value);

// Size operations (aliases to 64-bit on x64)
WC_AtomicSize wc_atomic_size_create(size_t initial_value);
void wc_atomic_size_destroy(WC_AtomicSize atomic);
size_t wc_atomic_size_load(WC_AtomicSize atomic);
void wc_atomic_size_store(WC_AtomicSize atomic, size_t value);
size_t wc_atomic_size_load_acquire(WC_AtomicSize atomic);
void wc_atomic_size_store_release(WC_AtomicSize atomic, size_t value);
bool wc_atomic_size_cas(WC_AtomicSize atomic, size_t expected, size_t desired);
size_t wc_atomic_size_exchange(WC_AtomicSize atomic, size_t value);
size_t wc_atomic_size_fetch_add(WC_AtomicSize atomic, size_t value);
size_t wc_atomic_size_fetch_sub(WC_AtomicSize atomic, size_t value);
size_t wc_atomic_size_increment(WC_AtomicSize atomic);
size_t wc_atomic_size_decrement(WC_AtomicSize atomic);