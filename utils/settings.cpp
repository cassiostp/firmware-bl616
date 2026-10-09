#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "settings.h"
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
        "# 1 shows a diagnostic line at the bottom of menus.\n"
        "diag=%d\n"
        "# 1 dims odd display lines in the cores (takes effect on the next game load).\n"
        "scanlines=%d\n",
        menu.c_str(), reset.c_str(), settings.reset_enabled ? 1 : 0,
        (unsigned long)settings.close_hold_ms, settings.diag ? 1 : 0,
        settings.scanlines ? 1 : 0);
    if (len <= 0 || len >= SETTINGS_BUF_SIZE)
        return false;

    if (f_open(&fcfg, path.c_str(), FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    UINT bw = 0;
    FRESULT r = f_write(&fcfg, cfgbuf, len, &bw);
    FRESULT rc = f_close(&fcfg);           // flushes the data
    return r == FR_OK && rc == FR_OK && bw == (UINT)len;
}
