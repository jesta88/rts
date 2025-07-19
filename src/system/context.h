#pragma once

typedef struct wc_context {
    struct SDL_Window* window;
    struct SDL_GPUDevice* gpu_device;
} wc_context;

