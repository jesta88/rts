#pragma once

#include "types.h"

typedef struct WC_Vertex
{
	float x, y, z, w;
	float screen_x, screen_y;
} WC_Vertex;

int wc_render_init(void);
void wc_render_draw(void);
void wc_render_quit(void);

VkInstance wc_render_get_instance(void);
VkPhysicalDevice wc_render_get_physical_device(void);
VkDevice wc_render_get_device(void);
VmaAllocator wc_render_get_allocator(void);