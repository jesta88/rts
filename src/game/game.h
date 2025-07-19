#pragma once

int wc_game_init(void);
void wc_game_update(double delta_time);
void wc_game_render(double interpolant);
void wc_game_quit(void);