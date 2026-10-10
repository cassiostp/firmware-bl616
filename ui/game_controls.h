#pragma once

#include <stdint.h>

// Actions requested by the controller. Set by controls_poll() or a menu,
// handled by the main menu loop.
enum GameAction {
    ACTION_NONE = 0,
    ACTION_GAME_MENU,   // open the game menu over the running game
    ACTION_RESUME,      // close the menus and go back to the game
    ACTION_RESET,       // reset the running game
    ACTION_CLOSE,       // close the game and show the main menu
};

extern volatile GameAction pending_action;

// Watch the controller combos and the MODE button. Call controls_poll() on
// every pass of an input loop, with the pad states (FPGA | USB) per player.
// in_game: the overlay is off and input goes to the core.
// game_loaded: a game (or a core from Cores) is loaded behind the menus.
// Returns true when an action has been set and the loop should exit.
// MODE reloads the FPGA from flash; when that's detected, this restarts the
// MCU (and so everything) and doesn't return.
bool controls_poll(uint16_t pad1, uint16_t pad2, bool in_game, bool game_loaded);

// True while at least 2 buttons of a combo are held, so menus can ignore
// navigation that's really the start of a combo.
bool combo_in_progress(uint16_t pad1, uint16_t pad2);

// Forget combo and MODE state, e.g. after the FPGA was reprogrammed (a new
// core is silent while it starts, which must not look like MODE).
void controls_reset(void);

// A game (or a core from Cores) is loaded behind the menus. In main.cpp.
bool game_loaded(void);

// Main menu "Options" screen
void menu_options(void);

// Game menu "Scanlines..." screen
struct Menu;
Menu *create_scanline_menu(void);

// Game menu "Video..." screen (scanlines, color, CRT mask, LCD grid)
Menu *create_video_menu(void);
