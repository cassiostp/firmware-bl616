#pragma once

#include <stdint.h>

// Actions requested while a game is running. Set by the in-game input loop,
// handled by the main menu loop.
enum GameAction {
    ACTION_NONE = 0,
    ACTION_MENU,        // menu combo held, or MODE held long: go to the main menu
    ACTION_RESET,       // reset combo held: reset the running game
    ACTION_RELOAD,      // MODE tapped: the FPGA reloaded from flash, reload core + game
};

extern volatile GameAction pending_action;

// In-game watcher. Call game_watch_start() when entering the in-game input
// loop, then game_watch_poll() every iteration with the current pad states.
// Returns true when an action has been set and the loop should exit.
void game_watch_start(int16_t game_core_id);
bool game_watch_poll(uint16_t pad1, uint16_t pad2);

// Main menu "Options" screen
void menu_options(void);
