#pragma once

#include <stdint.h>
#include <string>

// Joypad bits (R L X A RT LT DN UP START SELECT Y B), as reported by the FPGA
// and the USB gamepad code.
#define BTN_B       0x001
#define BTN_Y       0x002
#define BTN_SELECT  0x004
#define BTN_START   0x008
#define BTN_UP      0x010
#define BTN_DOWN    0x020
#define BTN_LEFT    0x040
#define BTN_RIGHT   0x080
#define BTN_A       0x100
#define BTN_X       0x200
#define BTN_L       0x400
#define BTN_R       0x800

#define COMBO_BUTTONS 3     // button combos are exactly this many buttons

// Persistent firmware settings, stored as key=value lines in <drv>tangcore.cfg
struct Settings {
    uint16_t menu_combo;        // opens the game menu in game, closes the menus outside
    uint16_t reset_combo;       // in game: resets the game, held close_hold_ms: closes it
    bool reset_enabled;         // the reset combo can be turned off
    uint32_t close_hold_ms;     // hold the reset combo this long to close the game
    bool diag;                  // show the diagnostic line at the bottom of menus
    bool scanlines;             // darken every 3rd output line in the cores (core_config bit 16)
    bool pause_in_menu;         // pause the game while the game menu is open (core_config bit 17)
};

extern Settings settings;

void settings_defaults(Settings &s);
void settings_load();           // call after the drive is mounted. missing file = defaults
bool settings_save();

// Send the video options to the running core. Pass true while the game menu is
// open, so the core pauses too if pause_in_menu is on. Keeps the low 16 bits.
void apply_core_config(bool game_menu_open);

int combo_count(uint16_t combo);
bool combo_valid(uint16_t combo, const char **why);
// "SELECT+START+L" for the config file, "SEL+START+L" for the OSD
std::string combo_to_string(uint16_t combo, bool short_names);
uint16_t combo_from_string(const char *s);
