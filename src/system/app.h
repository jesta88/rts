#pragma once

#include <stdbool.h>

typedef struct WC_AppCallbacks
{
	int (*init)(void);
	void (*update)(double delta_time);
	void (*render)(double interpolant);
	void (*quit)(void);
} WC_AppCallbacks;

int wc_app_init(const char* window_title, WC_AppCallbacks callbacks);
void wc_app_quit();

bool wc_app_is_running();

void wc_app_signal_shutdown();

void wc_app_update();

int wc_app_draw();

void* wc_app_get_window_handle();

void wc_app_get_window_size(int* width, int* height);

int wc_app_get_width();

int wc_app_get_height();

void wc_app_show_window();

float wc_app_get_dpi_scale();

bool wc_app_dpi_scale_was_changed();

void wc_app_set_size(int w, int h);

void wc_app_get_position(int* x, int* y);

void wc_app_set_position(int x, int y);

void wc_app_center_window();

bool wc_app_was_resized();

bool wc_app_was_moved();

bool wc_app_lost_focus();

bool wc_app_gained_focus();

bool wc_app_has_focus();

void wc_app_request_attention();

void wc_app_request_attention_continuously();

void wc_app_request_attention_cancel();

bool wc_app_was_minimized();

bool wc_app_was_maximized();

bool wc_app_minimized();

bool wc_app_maximized();

bool wc_app_was_restored();

bool wc_app_mouse_entered();

bool wc_app_mouse_exited();

bool wc_app_mouse_inside();
