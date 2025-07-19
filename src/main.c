#include "system/app.h"
#include "game/game.h"

int main(int argc, const char** argv)
{
	const WC_AppCallbacks callbacks = {
		.init = wc_game_init,
		.update = wc_game_update,
		.render = wc_game_render,
		.quit = wc_game_quit,
	};

	wc_app_init("Warcry", callbacks);

	while (wc_app_is_running())
	{
		wc_app_update();
	}

	wc_app_quit();

	return 0;
}
