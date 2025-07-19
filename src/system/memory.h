#pragma once

#include <stdlib.h>

//-------------------------------------------------------------------------------------------------
// General allocator

typedef struct wc_allocator
{
	void* (*alloc_func)(size_t size);
	void* (*calloc_func)(size_t count, size_t size);
	void* (*realloc_func)(void* memory, size_t size);
	void (*free_func)(void* memory);
} wc_allocator;

void* wc_malloc(size_t size);
void* wc_calloc(size_t count, size_t size);
void* wc_realloc(void* memory, size_t size);
void wc_free(void* memory);

void* wc_aligned_alloc(size_t size, size_t alignment);
void wc_aligned_free(void* memory, size_t alignment);

//-------------------------------------------------------------------------------------------------
// Arena allocator

typedef struct WC_ArenaRegion
{
	struct WC_ArenaRegion* next;
	size_t capacity;
	size_t used;
	uintptr_t* data;
} WC_ArenaRegion;

typedef struct WC_Arena
{
	WC_ArenaRegion* first;
	WC_ArenaRegion* last;
} WC_Arena;

int wc_arena_init(WC_Arena* arena, size_t initial_capacity);
void wc_arena_free(WC_Arena* arena);

void* wc_arena_alloc(WC_Arena* arena, size_t size);
void* wc_arena_alloc_aligned(WC_Arena* arena, size_t size, size_t alignment);
void wc_arena_reset(WC_Arena* arena);

//-------------------------------------------------------------------------------------------------
// Pool allocator

typedef struct wc_pool_allocator wc_pool_allocator;

wc_pool_allocator* wc_create_pool_allocator(size_t element_size, size_t element_count);
void wc_destroy_pool_allocator(wc_pool_allocator* allocator);
void* wc_pool_alloc(wc_pool_allocator* allocator);
void wc_pool_free(wc_pool_allocator* allocator, void* memory);
