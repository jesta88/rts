#include "system/app.h"
#include "game/game.h"
#include "system/profiler.h"

#include <SDL3/SDL_main.h>

int main(int argc, char** argv)
{
	const WC_AppCallbacks callbacks = {
		.init = wc_game_init,
		.update = wc_game_update,
		.render = wc_game_render,
		.quit = wc_game_quit,
	};

	profiler_init();
	wc_app_init("Warcry", callbacks);

	if (wc_game_init() != 0)
	{
		return -1;
	}

	while (wc_app_is_running())
	{
		profiler_frame_start();
		wc_app_update();
		profiler_frame_end();
	}

	wc_app_quit();
	profiler_shutdown();

	return 0;
}
