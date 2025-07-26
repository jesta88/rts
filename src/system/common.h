#pragma once

#if defined(_MSC_VER)
    #define WAR_FORCE_INLINE __forceinline
    #define WAR_ALIGN(x) __declspec(align(x))
    #define WAR_THREAD_LOCAL __declspec(thread)
    #define WAR_LIKELY(x) (x)
    #define WAR_UNLIKELY(x) (x)
#elif defined(__GNUC__) || defined(__clang__)
    #define WAR_FORCE_INLINE __attribute__((always_inline)) inline
    #define WAR_ALIGN(x) __attribute__((aligned(x)))
    #define WAR_THREAD_LOCAL __thread
    #define WAR_LIKELY(x) __builtin_expect(!!(x), 1)
    #define WAR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define WAR_FORCE_INLINE inline
    #define WAR_ALIGN(x)
    #define WAR_THREAD_LOCAL thread_local
    #define WAR_LIKELY(x) (x)
    #define WAR_UNLIKELY(x) (x)
#endif

#if defined(_MSC_VER)
    #undef assert

    #ifdef NDEBUG
        #define assert(expression) ((void) 0)
    #else
typedef unsigned short wchar_t;
__declspec(dllimport) void __cdecl _wassert(wchar_t const* _Message, wchar_t const* _File, unsigned _Line);

        #define _CRT_WIDE_(s) L##s
        #define _CRT_WIDE(s) _CRT_WIDE_(s)

        #define assert(expression)                                                                                                         \
            ((void) ((!!(expression)) || (_wassert(_CRT_WIDE(#expression), _CRT_WIDE(__FILE__), (unsigned) (__LINE__)), 0)))
    #endif
#endif

#define WAR_KB 1024
#define WAR_MB (WAR_KB * WAR_KB)
#define WAR_GB (WAR_MB * WAR_MB)
#define WAR_STRINGIFY_INTERNAL(...) #__VA_ARGS__
#define WAR_STRINGIFY(...) WAR_STRINGIFY_INTERNAL(__VA_ARGS__)

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef signed long long s64;
typedef float f32;
typedef double f64;

typedef u64 uptr;

#define NULL ((void*)0)

#define U64_MAX 18446744073709551615UL
#define U32_MAX 4294967295U
#define U16_MAX 65535U
#define U8_MAX 255U
#define U64_MIN 0UL
#define U32_MIN 0U
#define U16_MIN 0U
#define U8_MIN 0U

#define S8_MAX 127
#define S16_MAX 32767
#define S32_MAX 2147483647
#define S64_MAX 9223372036854775807L
#define S8_MIN (-S8_MAX - 1)
#define S16_MIN (-S16_MAX - 1)
#define S32_MIN (-S32_MAX - 1)
#define S64_MIN (-S64_MAX - 1)

static inline u64 war_align_up(u64 value, u64 alignment)
{
    assert((alignment & alignment - 1) == 0); // Power of 2
    return value + (alignment - 1) & ~(alignment - 1);
}

static inline uptr war_align_ptr(const uptr ptr, u64 alignment)
{
    assert((alignment & alignment - 1) == 0); // Power of 2
    return ptr + alignment - 1 & ~(alignment - 1);
}

#define war_min(x, y) ((x < y) ? x : y)
#define war_max(x, y) ((x > y) ? x : y)
