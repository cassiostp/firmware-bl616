#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "settings.h"
#include "overlay.h"
#include "utils.h"
#include "overlay.h"

Settings settings;

#define SETTINGS_FILE "tangcore.cfg"
#define SETTINGS_BUF_SIZE 1024

// FatFs objects used for USB drives must not be cached (see fbuf in main.cpp)
USB_NOCACHE_RAM_SECTION static FIL fcfg;
USB_NOCACHE_RAM_SECTION static char __attribute__((aligned(64))) cfgbuf[SETTINGS_BUF_SIZE];

static const struct {
    uint16_t bit;
    const char *name;       // config file
    const char *short_name; // OSD
} button_names[] = {
    {BTN_B, "B", "B"},
    {BTN_Y, "Y", "Y"},
    {BTN_SELECT, "SELECT", "SEL"},
    {BTN_START, "START", "START"},
    {BTN_UP, "UP", "UP"},
    {BTN_DOWN, "DOWN", "DOWN"},
    {BTN_LEFT, "LEFT", "LEFT"},
    {BTN_RIGHT, "RIGHT", "RIGHT"},
    {BTN_A, "A", "A"},
    {BTN_X, "X", "X"},
    {BTN_L, "L", "L"},
    {BTN_R, "R", "R"},
};
#define NUM_BUTTONS (sizeof(button_names) / sizeof(button_names[0]))

void settings_defaults(Settings &s) {
    s.menu_combo = BTN_SELECT | BTN_START | BTN_L;
    s.reset_combo = BTN_SELECT | BTN_START | BTN_R;
    s.reset_enabled = true;
    s.close_hold_ms = 3000;
    s.diag = false;
    s.scanlines = false;
    s.scanline_dark = 2;            // 75 %
    s.scanline_thick = false;
    s.scanline_full = false;
    s.video_brightness = 0;
    s.video_contrast = 0;
    s.video_saturation = 0;
    s.video_gamma = 0;
    s.crt_mask = 0;
    s.crt_mask_strength = 1;
    s.lcd_grid = false;
    s.lcd_grid_strength = 1;
    s.pause_in_menu = true;
}

int combo_count(uint16_t combo) {
    return __builtin_popcount(combo & 0xfff);
}

// A combo must be exactly COMBO_BUTTONS buttons and include Select or Start,
// so it doesn't fire during normal play.
bool combo_valid(uint16_t combo, const char **why) {
    const char *dummy;
    if (!why) why = &dummy;
    if (combo_count(combo) != COMBO_BUTTONS) {
        *why = "Need exactly 3 buttons";
        return false;
    }
    if (!(combo & (BTN_SELECT | BTN_START))) {
        *why = "Must include SEL or START";
        return false;
    }
    *why = NULL;
    return true;
}

std::string combo_to_string(uint16_t combo, bool short_names) {
    std::string s;
    // print in a friendly order: SELECT, START, then the rest
    static const uint16_t order[] = {BTN_SELECT, BTN_START, BTN_L, BTN_R, BTN_UP, BTN_DOWN,
                                     BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_X, BTN_Y};
    for (uint16_t bit : order) {
        if (!(combo & bit)) continue;
        for (unsigned i = 0; i < NUM_BUTTONS; i++) {
            if (button_names[i].bit == bit) {
                if (!s.empty()) s += "+";
                s += short_names ? button_names[i].short_name : button_names[i].name;
            }
        }
    }
    return s.empty() ? "NONE" : s;
}

// Parse "SELECT+START+L" (case-insensitive, short names accepted). 0 on error.
uint16_t combo_from_string(const char *s) {
    uint16_t combo = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '+') s++;
        const char *end = s;
        while (*end && *end != '+' && *end != ' ' && *end != '\t') end++;
        int len = end - s;
        if (len == 0) break;
        uint16_t bit = 0;
        for (unsigned i = 0; i < NUM_BUTTONS; i++) {
            if ((strlen(button_names[i].name) == (size_t)len && strncasecmp(s, button_names[i].name, len) == 0) ||
                (strlen(button_names[i].short_name) == (size_t)len && strncasecmp(s, button_names[i].short_name, len) == 0)) {
                bit = button_names[i].bit;
                break;
            }
        }
        if (!bit) return 0;
        combo |= bit;
        s = end;
    }
    return combo;
}

static std::string settings_path() {
    return std::string(drv) + SETTINGS_FILE;
}

static void apply_setting(Settings &s, const char *key, const char *val) {
    if (strcasecmp(key, "menu_combo") == 0) {
        uint16_t c = combo_from_string(val);
        if (combo_valid(c, NULL)) s.menu_combo = c;
    } else if (strcasecmp(key, "reset_combo") == 0) {
        uint16_t c = combo_from_string(val);
        if (combo_valid(c, NULL)) s.reset_combo = c;
    } else if (strcasecmp(key, "reset_enabled") == 0) {
        s.reset_enabled = strtol(val, NULL, 10) != 0;
    } else if (strcasecmp(key, "close_hold_ms") == 0) {
        long v = strtol(val, NULL, 10);
        if (v >= 1000 && v <= 10000) s.close_hold_ms = v;
    } else if (strcasecmp(key, "diag") == 0) {
        s.diag = strtol(val, NULL, 10) != 0;
    } else if (strcasecmp(key, "scanlines") == 0) {
        s.scanlines = strtol(val, NULL, 10) != 0;
    } else if (strcasecmp(key, "scanline_darkness") == 0) {
        long v = strtol(val, NULL, 10);
        if (v >= 25 && v <= 100) s.scanline_dark = (uint8_t)((v + 12) / 25 - 1);
    } else if (strcasecmp(key, "scanline_thick") == 0) {
        s.scanline_thick = strtol(val, NULL, 10) != 0;
    } else if (strcasecmp(key, "scanline_full") == 0) {
        s.scanline_full = strtol(val, NULL, 10) != 0;
    } else if (strcasecmp(key, "video_brightness") == 0) {
        long v = strtol(val, NULL, 10);
        if (v >= -4 && v <= 3) s.video_brightness = (int8_t)v;
    } else if (strcasecmp(key, "video_contrast") == 0) {
        long v = strtol(val, NULL, 10);
        if (v >= -4 && v <= 3) s.video_contrast = (int8_t)v;
    } else if (strcasecmp(key, "video_saturation") == 0) {
        long v = strtol(val, NULL, 10);
        if (v >= -4 && v <= 3) s.video_saturation = (int8_t)v;
    } else if (strcasecmp(key, "video_gamma") == 0) {
        long v = strtol(val, NULL, 10);
        if (v >= 0 && v <= 3) s.video_gamma = (uint8_t)v;
    } else if (strcasecmp(key, "crt_mask") == 0) {
        long v = strtol(val, NULL, 10);
        if (v >= 0 && v <= 3) s.crt_mask = (uint8_t)v;
    } else if (strcasecmp(key, "crt_mask_strength") == 0) {
        long v = strtol(val, NULL, 10);
        if (v >= 0 && v <= 3) s.crt_mask_strength = (uint8_t)v;
    } else if (strcasecmp(key, "lcd_grid") == 0) {
        s.lcd_grid = strtol(val, NULL, 10) != 0;
    } else if (strcasecmp(key, "lcd_grid_strength") == 0) {
        long v = strtol(val, NULL, 10);
        if (v >= 0 && v <= 3) s.lcd_grid_strength = (uint8_t)v;
    } else if (strcasecmp(key, "pause_in_menu") == 0) {
        s.pause_in_menu = strtol(val, NULL, 10) != 0;
    }
}

void settings_load() {
    Settings s;
    settings_defaults(s);
    std::string path = settings_path();
    UINT br = 0;
    if (f_open(&fcfg, path.c_str(), FA_READ) == FR_OK) {
        f_read(&fcfg, cfgbuf, SETTINGS_BUF_SIZE - 1, &br);
        f_close(&fcfg);
        cfgbuf[br] = '\0';
        if (br == SETTINGS_BUF_SIZE - 1) {      // too long: drop the cut-off last line
            char *nl = strrchr(cfgbuf, '\n');
            if (nl) *nl = '\0';
        }

        // parse key=value lines, '#' starts a comment
        char *line = cfgbuf;
        if ((uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF)
            line += 3;                          // UTF-8 BOM from Windows editors
        while (line && *line) {
            char *next = strpbrk(line, "\r\n");
            if (next) *next++ = '\0';
            char *hash = strchr(line, '#');
            if (hash) *hash = '\0';
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *key = line, *val = eq + 1;
                while (*key == ' ' || *key == '\t') key++;
                for (char *e = eq - 1; e >= key && (*e == ' ' || *e == '\t'); e--) *e = '\0';
                while (*val == ' ' || *val == '\t') val++;
                apply_setting(s, key, val);
            }
            line = next;
        }
        overlay_status("Settings loaded from %s", path.c_str());
    }
    if (s.menu_combo == s.reset_combo)      // keep the two combos distinct
        settings_defaults(s);
    settings = s;
}

bool settings_save() {
    std::string path = settings_path();
    std::string menu = combo_to_string(settings.menu_combo, false);
    std::string reset = combo_to_string(settings.reset_combo, false);
    int len = snprintf(cfgbuf, SETTINGS_BUF_SIZE,
        "# TangCore settings. Buttons: B Y SELECT START UP DOWN LEFT RIGHT A X L R\n"
        "# In game: menu_combo opens the game menu. reset_combo resets the game;\n"
        "# held for close_hold_ms it closes the game. reset_enabled=0 turns it off.\n"
        "menu_combo=%s\n"
        "reset_combo=%s\n"
        "reset_enabled=%d\n"
        "close_hold_ms=%lu\n"
        "# 1 shows a diagnostic line at the top of menus.\n"
        "diag=%d\n"
        "# Scanlines: 1 = on; darkness 25, 50, 75 or 100 (%%); thick 1 = thick lines;\n"
        "# full 1 = full-size picture (0 = integer scale, evenly spaced lines).\n"
        "scanlines=%d\n"
        "scanline_darkness=%d\n"
        "scanline_thick=%d\n"
        "scanline_full=%d\n"
        "# Video filters: brightness/contrast/saturation -4..3, gamma 0..3\n"
        "# (off, darker, brighter, CRT), mask 0..3 (off, grille, slot, dot),\n"
        "# strengths 0..3, grid 1 = on.\n"
        "video_brightness=%d\n"
        "video_contrast=%d\n"
        "video_saturation=%d\n"
        "video_gamma=%d\n"
        "crt_mask=%d\n"
        "crt_mask_strength=%d\n"
        "lcd_grid=%d\n"
        "lcd_grid_strength=%d\n"
        "# 0 keeps the game running while a menu is shown over it.\n"
        "pause_in_menu=%d\n",
        menu.c_str(), reset.c_str(), settings.reset_enabled ? 1 : 0,
        (unsigned long)settings.close_hold_ms, settings.diag ? 1 : 0,
        settings.scanlines ? 1 : 0, scanline_dark_percent(settings.scanline_dark),
        settings.scanline_thick ? 1 : 0, settings.scanline_full ? 1 : 0,
        settings.video_brightness, settings.video_contrast, settings.video_saturation,
        settings.video_gamma, settings.crt_mask, settings.crt_mask_strength,
        settings.lcd_grid ? 1 : 0, settings.lcd_grid_strength,
        settings.pause_in_menu ? 1 : 0);
    if (len <= 0 || len >= SETTINGS_BUF_SIZE)
        return false;

    if (f_open(&fcfg, path.c_str(), FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    UINT bw = 0;
    FRESULT r = f_write(&fcfg, cfgbuf, len, &bw);
    FRESULT rc = f_close(&fcfg);           // flushes the data
    return r == FR_OK && rc == FR_OK && bw == (UINT)len;
}

// Drive the core_config option bits from the settings. The game pauses while
// any menu is shown over it. The low 16 bits are core specific (e.g. GBA's
// prefetch delay), so keep whatever the firmware last sent there.
static Preview preview;

void apply_core_config() {
    bool pause = core_running &&
                 (preview == PREVIEW_PAUSED || (settings.pause_in_menu && _overlay_on));
    bool mute = core_running && preview == PREVIEW_LIVE;
    set_core_config((get_core_config() & 0xffff) |
                    (settings.scanlines ? CORE_CFG_SCANLINES : 0) |
                    ((uint32_t)(settings.scanline_dark & 3) << CORE_CFG_SCANLINE_DARK_SHIFT) |
                    (settings.scanline_thick ? CORE_CFG_SCANLINE_THICK : 0) |
                    (settings.scanline_full ? CORE_CFG_SCANLINE_FULL : 0) |
                    (pause ? CORE_CFG_MENU_PAUSE : 0) |
                    (mute ? CORE_CFG_MUTE_PADS : 0));
    // The filters go out with every core_config: same value semantics (they
    // may change at any time), so every apply path covers both frames.
    set_video_config(build_video_config());
}

// Pack the filter settings into the video_config word (frame 0x13). The
// signed -4..+3 fields go out as 3-bit two's complement.
uint32_t build_video_config() {
    return ((uint32_t)(settings.video_brightness & 7) << VIDEO_CFG_BRIGHT_SHIFT) |
           ((uint32_t)(settings.video_contrast & 7) << VIDEO_CFG_CONTRAST_SHIFT) |
           ((uint32_t)(settings.video_saturation & 7) << VIDEO_CFG_SATUR_SHIFT) |
           ((uint32_t)(settings.video_gamma & 3) << VIDEO_CFG_GAMMA_SHIFT) |
           ((uint32_t)(settings.crt_mask & 3) << VIDEO_CFG_MASK_SHIFT) |
           ((uint32_t)(settings.crt_mask_strength & 3) << VIDEO_CFG_MASK_STRENGTH_SHIFT) |
           (settings.lcd_grid ? VIDEO_CFG_LCD_GRID : 0) |
           ((uint32_t)(settings.lcd_grid_strength & 3) << VIDEO_CFG_GRID_STRENGTH_SHIFT);
}

void core_config_preview(Preview p) {
    preview = p;
    apply_core_config();
}

int scanline_dark_percent(uint8_t dark) {
    return 25 * ((dark & 3) + 1);
}
