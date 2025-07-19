#pragma once

typedef struct WC_Vertex
{
	float x, y, z, w;
	float screen_x, screen_y;
} WC_Vertex;

int wc_render_init(void);
void wc_render_draw(void);
void wc_render_quit(void);