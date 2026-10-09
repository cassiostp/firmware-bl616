#include <string>
#include <memory>

extern "C" {
#include "bl616_glb.h"
#include "bl616_hbn.h"
}

#include "game_controls.h"
#include "settings.h"
#include "menu_manager.h"
#include "overlay.h"
#include "utils.h"

volatile GameAction pending_action = ACTION_NONE;

/////////////////////////////////////////////////////////////////////////////////
// Controller combos and the MODE (FPGA reconfig) button

#define MENU_HOLD_MS      100     // menu combo: held this long (debounce)
#define QUIT_HOLD_MS      3000    // quit combo: held this long
#define POLL_MS_GAME      250     // how often to ask the FPGA for its core ID
#define POLL_MS_MENU      500
#define MODE_SILENT_MS    600     // FPGA silent this long, then back: MODE was pressed

static uint16_t combo_held;         // which combo is being held (0 = none)
static uint64_t combo_since;        // when it started being held
static uint64_t last_poll;
static bool seen_answer;            // the FPGA answered since the last reset
static uint64_t silent_since;       // start of the first unanswered poll (0 = answering)

void controls_reset(void) {
    combo_held = 0;
    combo_since = 0;
    last_poll = bflb_mtimer_get_time_ms();
    seen_answer = false;
    silent_since = 0;
}

static bool pressed(uint16_t pad1, uint16_t pad2, uint16_t combo) {
    return combo && (((pad1 & combo) == combo) || ((pad2 & combo) == combo));
}

static bool check_combos(uint16_t pad1, uint16_t pad2, bool in_game, bool game_loaded) {
    uint16_t held = 0;
    if (pressed(pad1, pad2, settings.menu_combo))
        held = settings.menu_combo;
    else if (pressed(pad1, pad2, settings.quit_combo))
        held = settings.quit_combo;

    uint64_t now = bflb_mtimer_get_time_ms();
    if (held != combo_held) {
        combo_held = held;
        combo_since = now;
        return false;
    }
    if (!held || !game_loaded)
        return false;

    GameAction action = ACTION_NONE;
    if (held == settings.menu_combo && now - combo_since >= MENU_HOLD_MS)
        action = in_game ? ACTION_GAME_MENU : ACTION_RESUME;
    else if (held == settings.quit_combo && in_game && now - combo_since >= QUIT_HOLD_MS)
        action = ACTION_QUIT;
    if (action == ACTION_NONE)
        return false;

    // act now, and keep the buttons from reaching menus or the core until
    // they're let go
    suppress_held_buttons();
    combo_held = 0;
    pending_action = action;
    dprint("Combo %04x: action %d", held, action);
    return true;
}

// MODE is wired to the FPGA's RECONFIG_N, so firmware can't see the button.
// What it can see: the FPGA stops answering while it reloads from flash, then
// answers again. A single missed reply is normal (one can be dropped while
// the FPGA sends a joypad frame), so only a longer silence counts. MODE then
// restarts everything, like a power cycle.
static void check_mode_button(bool in_game) {
    uint64_t poll_start = bflb_mtimer_get_time_ms();
    if (poll_start - last_poll < (in_game ? POLL_MS_GAME : POLL_MS_MENU))
        return;
    last_poll = poll_start;

    int16_t id = get_core_id();         // up to 200ms when the FPGA is silent
    if (id < 0) {
        if (seen_answer && !silent_since)
            silent_since = poll_start;
        return;
    }
    uint64_t now = bflb_mtimer_get_time_ms();
    if (silent_since && now - silent_since >= MODE_SILENT_MS) {
        dprint("FPGA was silent for %lu ms: MODE pressed, restarting",
               (unsigned long)(now - silent_since));
        overlay(1);
        overlay_status("Restarting...");
        delay(50);
        GLB_SW_System_Reset();
    }
    seen_answer = true;
    silent_since = 0;
}

bool controls_poll(uint16_t pad1, uint16_t pad2, bool in_game, bool game_loaded) {
    if (pending_action != ACTION_NONE)
        return true;
    if (check_combos(pad1, pad2, in_game, game_loaded))
        return true;
    check_mode_button(in_game);
    return false;
}

/////////////////////////////////////////////////////////////////////////////////
// Options screen

// Show a prompt and record the next combo of exactly COMBO_BUTTONS buttons held
// steady for a second. Returns 0 if cancelled (nothing held for 10 seconds).
static uint16_t capture_combo(const char *what) {
    overlay_clear();
    overlay_cursor(0, 10);
    //              01234567890123456789012345678901
    overlay_printf("  Set %s", what);
    overlay_cursor(0, 12);
    overlay_printf("  Hold 3 buttons together");
    overlay_cursor(0, 13);
    overlay_printf("  for 1 second, including");
    overlay_cursor(0, 14);
    overlay_printf("  SEL or START.");
    overlay_cursor(0, 16);
    overlay_printf("  Wait 10s to cancel.");

    suppress_held_buttons();        // ignore the button that opened this screen

    uint64_t start = bflb_mtimer_get_time_ms();
    uint64_t steady_since = start;
    uint16_t last = 0;
    while (1) {
        uint64_t now = bflb_mtimer_get_time_ms();
        uint16_t joy1, joy2, hid1, hid2;
        get_joypad_states(&joy1, &joy2, &hid1, &hid2);
        uint16_t p1 = (joy1 | hid1) & 0xfff, p2 = (joy2 | hid2) & 0xfff;
        uint16_t cur = combo_count(p1) >= combo_count(p2) ? p1 : p2;

        if (cur != last) {
            last = cur;
            steady_since = now;
            overlay_cursor(0, 18);
            overlay_printf("  %-28s", cur ? combo_to_string(cur, true).c_str() : "");
        }
        if (cur)
            start = now;            // any input restarts the cancel timeout
        if (combo_count(cur) == COMBO_BUTTONS && now - steady_since >= 1000) {
            overlay_cursor(0, 18);
            overlay_printf("  %-28s", (combo_to_string(cur, true) + " - got it").c_str());
            suppress_held_buttons();    // releasing them mustn't act on the menu
            delay(500);                 // long enough to read
            return cur;
        }
        if (now - start >= 10000)
            return 0;
        delay(20);
    }
}

// Reboot the BL616 into its ROM bootloader, as if BOOT were held at power-up,
// so the firmware can be flashed without opening the case. The ROM reads the
// boot selection from HBN_RSV2, which survives a software reset but not a
// power cut.
static void reboot_to_flash_mode(void) {
    overlay_clear();
    overlay_cursor(0, 9);
    //              01234567890123456789012345678901
    overlay_printf("  --- Flash mode ---");
    overlay_cursor(0, 11);
    overlay_printf("  Ready to be flashed.");
    overlay_cursor(0, 13);
    overlay_printf("  Keep the power connected,");
    overlay_cursor(0, 14);
    overlay_printf("  and connect the BL616 USB-C");
    overlay_cursor(0, 15);
    overlay_printf("  port to the PC.");
    overlay_cursor(0, 17);
    overlay_printf("  Power-cycle to cancel.");
    delay(100);                     // let the UART drain
    HBN_Set_User_Boot_Config(1);    // 1: boot from interface (download mode)
    GLB_SW_System_Reset();
}

struct FlashModeMenu: Menu {
    void render() override {
        overlay_clear();
        overlay_cursor(0, 9);
        //              01234567890123456789012345678901
        overlay_printf("  --- Flash mode ---");
        overlay_cursor(0, 11);
        overlay_printf("  Restarts the MCU ready to be");
        overlay_cursor(0, 12);
        overlay_printf("  flashed from a PC, without");
        overlay_cursor(0, 13);
        overlay_printf("  the BOOT button. Keep the");
        overlay_cursor(0, 14);
        overlay_printf("  power connected.");
        overlay_cursor(2, 16);
        overlay_printf("Restart in flash mode");
        overlay_cursor(2, 17);
        overlay_printf("<< Cancel");
    }

    std::vector<int> get_options() override {
        return {16, 17};
    }

    bool on_choose(int idx) override {
        if (idx == 0)
            reboot_to_flash_mode();     // doesn't return
        return true;
    }
};

struct OptionsMenu: Menu {
    Settings edit;
    std::string message;

    OptionsMenu() : edit(settings) {}

    void render() override {
        overlay_clear();
        overlay_cursor(0, 7);
        //              01234567890123456789012345678901
        overlay_printf("         --- Options ---");
        overlay_cursor(2, 10);
        overlay_printf("Menu: %s", combo_to_string(edit.menu_combo, true).c_str());
        overlay_cursor(2, 11);
        overlay_printf("Quit: %s", combo_to_string(edit.quit_combo, true).c_str());
        overlay_cursor(2, 12);
        overlay_printf("Diagnostics: %s", edit.diag ? "ON" : "OFF");
        overlay_cursor(2, 13);
        overlay_printf("Flash mode...");
        overlay_cursor(2, 15);
        overlay_printf("Save");
        overlay_cursor(2, 16);
        overlay_printf("<< Back");
        overlay_cursor(2, 19);
        overlay_printf("In game:");
        overlay_cursor(2, 20);
        overlay_printf(" Menu combo: game menu");
        overlay_cursor(2, 21);
        overlay_printf(" Hold quit combo 3s: quit");
        if (!message.empty()) {
            overlay_cursor(2, 23);
            overlay_printf("%s", message.c_str());
        }
    }

    std::vector<int> get_options() override {
        return {10, 11, 12, 13, 15, 16};
    }

    bool set_combo(uint16_t *target, uint16_t other, const char *what) {
        uint16_t c = capture_combo(what);
        const char *why = NULL;
        if (!c)
            message = "Cancelled";
        else if (!combo_valid(c, &why))
            message = why;
        else if (c == other)
            message = "Same as the other combo";
        else {
            *target = c;
            message = "";
        }
        do_redraw();
        return false;
    }

    bool on_choose(int idx) override {
        switch (idx) {
        case 0:
            return set_combo(&edit.menu_combo, edit.quit_combo, "menu combo");
        case 1:
            return set_combo(&edit.quit_combo, edit.menu_combo, "quit combo");
        case 2:
            edit.diag = !edit.diag;
            message = "";
            do_redraw();
            suppress_held_buttons();
            return false;
        case 3:
            suppress_held_buttons();
            push_menu(std::unique_ptr<Menu>(new FlashModeMenu()));
            return false;
        case 4:
            settings = edit;
            message = settings_save() ? "Saved" : "Save failed. Read-only drive?";
            do_redraw();
            suppress_held_buttons();
            return false;
        default:
            return true;    // back
        }
    }
};

void menu_options(void) {
    menu_clear();
    push_menu(std::unique_ptr<Menu>(new OptionsMenu()));
    menu_current()->do_redraw();
    suppress_held_buttons();    // the button that opened Options
    menu_input_loop();
    menu_clear();
}
