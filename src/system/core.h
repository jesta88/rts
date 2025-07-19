#pragma once

#include <stdint.h>

#define WC_KB 1024
#define WC_MB (WC_KB * WC_KB)
#define WC_GB (WC_MB * WC_MB)
#define WC_STRINGIFY_INTERNAL(...) #__VA_ARGS__
#define WC_STRINGIFY(...) WC_STRINGIFY_INTERNAL(__VA_ARGS__)
#define WC_OFFSET_OF(T, member) ((int)((uintptr_t)(&(((T*)0)->member))))
#define WC_ALIGN_TRUNCATE(v, n) ((v) & ~((n) - 1))
#define WC_ALIGN_FORWARD(v, n) WC_ALIGN_TRUNCATE((v) + (n) - 1, (n))
#define WC_ALIGN_TRUNCATE_PTR(p, n) ((void*)WC_ALIGN_TRUNCATE((uintptr_t)(p), n))
#define WC_ALIGN_FORWARD_PTR(p, n) ((void*)WC_ALIGN_FORWARD((uintptr_t)(p), n))
#define WC_ALIGN_BACKWARD(v, n) WC_ALIGN_TRUNCATE((v), (n))
#define WC_ALIGN_BACKWARD_PTR(p, n) ((void*)WC_ALIGN_BACKWARD((uintptr_t)(p), n))