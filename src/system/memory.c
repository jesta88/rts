#include "memory.h"

#include "SDL3/SDL_log.h"
#include "core.h"

#include <mimalloc.h>

void* wc_malloc(const size_t size)
{
	return mi_malloc(size);
}

void* wc_calloc(const size_t count, const size_t size)
{
	return mi_calloc(count, size);
}

void* wc_realloc(void* const memory, const size_t size)
{
	return mi_realloc(memory, size);
}

void wc_free(void* const memory)
{
	mi_free(memory);
}

void* wc_aligned_alloc(const size_t size, const size_t alignment)
{
	return mi_malloc_aligned(size, alignment);
}

void wc_aligned_free(void* memory, const size_t alignment)
{
	mi_free_aligned(memory, alignment);
}

//-------------------------------------------------------------------------------------------------

#ifndef WC_ARENA_DEFAULT_REGION_SIZE
#define WC_ARENA_DEFAULT_REGION_SIZE (4 * 1024) // Size of the smallest page on Windows
#endif

#ifndef WC_ARENA_DEFAULT_ALIGNMENT
#define WC_ARENA_DEFAULT_ALIGNMENT 32 // Required for AVX2
#endif

static WC_ArenaRegion* wc_arena_new_region(const size_t capacity)
{
	const size_t total_size = sizeof(WC_ArenaRegion) + capacity;
	void* raw_memory = wc_malloc(total_size);
	if (!raw_memory)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to allocate memory for new region");
		return NULL;
	}

	WC_ArenaRegion* region = raw_memory;
	region->next = NULL;
	region->capacity = capacity;
	region->used = 0;
	// The data block starts immediately after the Region struct.
	region->data = (uintptr_t*)((char*)raw_memory + sizeof(WC_ArenaRegion));

	return region;
}

int wc_arena_init(WC_Arena* arena, size_t initial_capacity)
{
	if (initial_capacity == 0)
	{
		initial_capacity = WC_ARENA_DEFAULT_REGION_SIZE;
	}
	WC_ArenaRegion* first_region = wc_arena_new_region(initial_capacity);
	if (first_region == NULL)
	{
		return -1;
	}
	arena->first = first_region;
	arena->last = first_region;
	return 0;
}

void wc_arena_free(WC_Arena* arena)
{
	WC_ArenaRegion* current = arena->first;
	while (current != NULL)
	{
		WC_ArenaRegion* next = current->next;
		wc_free(current);
		current = next;
	}

	arena->first = NULL;
	arena->last = NULL;
}

void* wc_arena_alloc_aligned(WC_Arena* arena, const size_t size, const size_t alignment)
{
	if (size == 0)
	{
		return NULL;
	}

	WC_ArenaRegion* region = arena->last;

	// First, try to allocate from the last region used. This is the fast path.
	// If that fails, walk the list of regions to find one with space.
	// This logic allows arena_reset to work by reusing the existing chain of regions.
	while (region)
	{
		const uintptr_t current_ptr = (uintptr_t)region->data + region->used;
		const uintptr_t aligned_ptr = WC_ALIGN_FORWARD(current_ptr, alignment);
		const size_t total_needed = size + (aligned_ptr - current_ptr);

		if (region->used + total_needed <= region->capacity)
		{
			// Found a region with enough space.
			region->used += total_needed;
			arena->last = region; // Cache this region for the next allocation.
			return (void*)aligned_ptr;
		}
		// Not enough space in this region, try the next one.
		region = region->next;
	}

	// If we get here, no existing region has enough space.
	// We must allocate a new one.
	const size_t new_region_capacity = size > WC_ARENA_DEFAULT_REGION_SIZE ? size : WC_ARENA_DEFAULT_REGION_SIZE;
	WC_ArenaRegion* new_region = wc_arena_new_region(new_region_capacity);
	if (!new_region)
	{
		return NULL; // Allocation failed.
	}

	// Find the true end of the list to append the new region.
	WC_ArenaRegion* end_of_list = arena->first;
	while (end_of_list->next)
	{
		end_of_list = end_of_list->next;
	}
	end_of_list->next = new_region;

	// Now, allocate from the new region.
	const uintptr_t aligned_ptr = WC_ALIGN_FORWARD((uintptr_t)new_region->data, alignment);
	new_region->used = size + (aligned_ptr - (uintptr_t)new_region->data);
	arena->last = new_region; // The new region is now the last one used.

	return (void*)aligned_ptr;
}

// Default allocation with arena's default alignment
void* wc_arena_alloc(WC_Arena* arena, const size_t size)
{
	return wc_arena_alloc_aligned(arena, size, WC_ARENA_DEFAULT_ALIGNMENT);
}

void wc_arena_reset(WC_Arena* arena)
{
	WC_ArenaRegion* current = arena->first;
	while (current)
	{
		current->used = 0;
		current = current->next;
	}
	arena->last = arena->first;
}

//-------------------------------------------------------------------------------------------------

#define WC_ALIGN_TRUNCATE(v, n) ((v) & ~((n) - 1))
#define WC_ALIGN_FORWARD(v, n) WC_ALIGN_TRUNCATE((v) + (n) - 1, (n))

typedef struct wc_memory_block
{
	uint8_t* memory;
	struct wc_memory_block* next;
} wc_memory_block;

struct WC_Pool
{
	size_t element_size;
	size_t block_size;
	size_t alignment;
	void* free_list;
	wc_memory_block* blocks;
	size_t element_count_per_block;
};

WC_Pool* wc_create_pool_allocator(size_t element_size, const size_t element_count)
{
	const size_t default_alignment = 16;
	element_size = element_size > sizeof(void*) ? element_size : sizeof(void*);
	element_size = WC_ALIGN_FORWARD(element_size, default_alignment);
	const size_t block_size = element_size * element_count;

	WC_Pool* pool = wc_aligned_alloc(sizeof(WC_Pool), default_alignment);
	pool->element_size = element_size;
	pool->block_size = block_size;
	pool->alignment = default_alignment;
	pool->free_list = NULL;
	pool->element_count_per_block = element_count;

	// Allocate the first block and initialize the free list.
	wc_memory_block* block = wc_aligned_alloc(sizeof(wc_memory_block), default_alignment);
	block->memory = (uint8_t*)wc_aligned_alloc(block_size, default_alignment);
	block->next = NULL;
	pool->blocks = block;
	pool->free_list = block->memory;
	for (size_t i = 0; i < element_count - 1; ++i)
	{
		void** element = (void**)(block->memory + element_size * i);
		void* next = block->memory + element_size * (i + 1);
		*element = next;
	}
	void** last_element = (void**)(block->memory + element_size * (element_count - 1));
	*last_element = NULL;

	return pool;
}

void wc_destroy_pool_allocator(WC_Pool* allocator)
{
	wc_memory_block* block = allocator->blocks;
	while (block)
	{
		wc_memory_block* next = block->next;
		wc_aligned_free(block->memory, allocator->alignment);
		wc_aligned_free(block, allocator->alignment);
		block = next;
	}
	wc_aligned_free(allocator, allocator->alignment);
}

void* wc_pool_alloc(WC_Pool* allocator)
{
	// Try to allocate from the free list first.
	if (allocator->free_list)
	{
		void* mem = allocator->free_list;
		allocator->free_list = *(void**)allocator->free_list;
		return mem;
	}

	// If no free elements, allocate a new block and initialize its free list.
	wc_memory_block* new_block = wc_aligned_alloc(sizeof(wc_memory_block), allocator->alignment);
	new_block->memory = (uint8_t*)wc_aligned_alloc(allocator->block_size, allocator->alignment);
	new_block->next = allocator->blocks;
	allocator->blocks = new_block;
	allocator->free_list = new_block->memory;
	for (size_t i = 0; i < allocator->element_count_per_block - 1; ++i)
	{
		void** element = (void**)(new_block->memory + allocator->element_size * i);
		void* next = new_block->memory + allocator->element_size * (i + 1);
		*element = next;
	}
	void** last_element = (void**)(new_block->memory + allocator->element_size * (allocator->element_count_per_block - 1));
	*last_element = NULL;

	return wc_pool_alloc(allocator);
}

void wc_pool_free(WC_Pool* allocator, void* memory)
{
	*(void**)memory = allocator->free_list;
	allocator->free_list = memory;
}
