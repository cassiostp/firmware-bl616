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
    bool diag;                  // show the diagnostic line at the top of menus
    bool scanlines;             // darken the gaps between picture lines in the cores (core_config bit 16)
    uint8_t scanline_dark;      // 0..3: 25, 50, 75, 100 % darker (core_config bits 19:18)
    bool scanline_thick;        // thick lines instead of thin ones (core_config bit 20)
    bool scanline_full;         // full-size picture, lines at a fixed pitch (core_config bit 21)
    int8_t video_brightness;    // -4..+3: add 16*n to every channel (video_config bits 2:0)
    int8_t video_contrast;      // -4..+3: scale around 128 (video_config bits 5:3)
    int8_t video_saturation;    // -4..+3: blend towards luma, -4 is grey (video_config bits 8:6)
    uint8_t video_gamma;        // 0..3: off, darker, brighter, CRT (video_config bits 10:9)
    uint8_t crt_mask;           // 0..3: off, grille, slot, dot (video_config bits 12:11)
    uint8_t crt_mask_strength;  // 0..3: the mask dims 1/4, 3/8, 1/2, 5/8 (video_config bits 14:13)
    bool lcd_grid;              // handheld pixel grid (video_config bit 15)
    uint8_t lcd_grid_strength;  // 0..3: the grid dims 1/8, 1/4, 3/8, 1/2 (video_config bits 17:16)
    uint8_t smoothing;          // 0..2: off, sharp, soft (video_config bits 19:18)
    bool pause_in_menu;         // pause the game while a menu is shown over it (core_config bit 17)
};

extern Settings settings;

void settings_defaults(Settings &s);
void settings_load();           // call after the drive is mounted. missing file = defaults
bool settings_save();

// Send the options to the running core: scanlines, and pause while a menu is
// shown over a running game (if pause_in_menu). Keeps the low 16 bits.
// overlay() calls it on every change.
void apply_core_config();

// Build the video_config word (frame 0x13) from the filter settings.
uint32_t build_video_config();

// True if the running core offers the LCD grid (GBA, Game Gear).
bool lcd_grid_offered();

// The scanline preview shows the running game with the menu hidden: paused,
// or running with the pads muted (cores that can't show a paused frame
// without the menu).
enum Preview { PREVIEW_OFF, PREVIEW_PAUSED, PREVIEW_LIVE };
void core_config_preview(Preview p);

// "75%" for scanline_dark
int scanline_dark_percent(uint8_t dark);

int combo_count(uint16_t combo);
bool combo_valid(uint16_t combo, const char **why);
// "SELECT+START+L" for the config file, "SEL+START+L" for the OSD
std::string combo_to_string(uint16_t combo, bool short_names);
uint16_t combo_from_string(const char *s);
