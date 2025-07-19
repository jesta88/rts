#include "game.h"
#include "../system/app.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_render.h>

static SDL_Renderer* s_renderer;
SDL_FRect g_playerRect = {100.0f, 100.0f, 50.0f, 50.0f};
// Store previous state for interpolation
float g_previousX = 100.0f;
float g_previousY = 100.0f;

int wc_game_init()
{
	SDL_Window* window = wc_app_get_window_handle();
	s_renderer = SDL_CreateRenderer(window, NULL);
	if (!s_renderer)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateRenderer: %s", SDL_GetError());
		return -1;
	}
	return 0;
}

void wc_game_update(const double delta_time)
{
	// Store the current state before updating. This is needed for interpolation.
	g_previousX = g_playerRect.x;
	g_previousY = g_playerRect.y;

	const bool* keystate = SDL_GetKeyboardState(NULL);
	const float velocity = 200.0f; // pixels per second

	if (keystate[SDL_SCANCODE_RIGHT]) {
		g_playerRect.x += velocity * (float)delta_time;
	}
	if (keystate[SDL_SCANCODE_LEFT]) {
		g_playerRect.x -= velocity * (float)delta_time;
	}
	if (keystate[SDL_SCANCODE_DOWN]) {
		g_playerRect.y += velocity * (float)delta_time;
	}
	if (keystate[SDL_SCANCODE_UP]) {
		g_playerRect.y -= velocity * (float)delta_time;
	}

	// Keep the player on screen
	int w, h;
	SDL_GetRenderOutputSize(s_renderer, &w, &h);
	if (g_playerRect.x < 0) g_playerRect.x = 0;
	if (g_playerRect.y < 0) g_playerRect.y = 0;
	if (g_playerRect.x > (float)w - g_playerRect.w) g_playerRect.x = (float)w - g_playerRect.w;
	if (g_playerRect.y > (float)h - g_playerRect.h) g_playerRect.y = (float)h - g_playerRect.h;
}

void wc_game_render(const double interpolant)
{
	SDL_SetRenderDrawColor(s_renderer, 34, 39, 46, 255);
	SDL_RenderClear(s_renderer);

	SDL_FRect renderRect = g_playerRect;
	renderRect.x = (float)(g_playerRect.x * interpolant + g_previousX * (1.0 - interpolant));
	renderRect.y = (float)(g_playerRect.y * interpolant + g_previousY * (1.0 - interpolant));

	// Draw the interpolated player rectangle (e.g., in orange)
	SDL_SetRenderDrawColor(s_renderer, 253, 126, 20, 255);
	SDL_RenderFillRect(s_renderer, &renderRect);

	SDL_RenderPresent(s_renderer);
}

void wc_game_quit()
{
    SDL_DestroyRenderer(s_renderer);
}
