#include <string>
#include <memory>

#include "game_controls.h"
#include "settings.h"
#include "menu_manager.h"
#include "overlay.h"
#include "utils.h"

volatile GameAction pending_action = ACTION_NONE;

/////////////////////////////////////////////////////////////////////////////////
// In-game watcher: button combos and the MODE (FPGA reconfig) button

#define COMBO_HOLD_MS     300     // combo must be held this long to trigger
#define COMBO_RELEASE_MS  3000    // max wait for the combo to be released
#define CORE_POLL_MS      250     // how often to ask the FPGA for its core ID

static int16_t watch_core;          // core ID expected while the game runs
static uint64_t combo_since;        // when the current combo started being held
static uint16_t combo_held;         // which combo is being held (0 = none)
static uint64_t last_poll;
static uint64_t silent_since;       // when the FPGA stopped answering (0 = answering)

void game_watch_start(int16_t game_core_id) {
    watch_core = game_core_id;
    combo_since = 0;
    combo_held = 0;
    last_poll = bflb_mtimer_get_time_ms();
    silent_since = 0;
}

static bool pressed(uint16_t pad1, uint16_t pad2, uint16_t combo) {
    return combo && (((pad1 & combo) == combo) || ((pad2 & combo) == combo));
}

// wait until no pad holds any button of `combo`, so the release isn't seen
// as menu input
static void wait_combo_release(uint16_t combo) {
    uint64_t start = bflb_mtimer_get_time_ms();
    while (bflb_mtimer_get_time_ms() - start < COMBO_RELEASE_MS) {
        uint16_t joy1, joy2, hid1, hid2;
        get_joypad_states(&joy1, &joy2, &hid1, &hid2);
        if (!(((joy1 | hid1) | (joy2 | hid2)) & combo))
            break;
        delay(20);
    }
}

static bool check_combos(uint16_t pad1, uint16_t pad2) {
    uint16_t held = 0;
    if (pressed(pad1, pad2, settings.menu_combo))
        held = settings.menu_combo;
    else if (pressed(pad1, pad2, settings.reset_combo))
        held = settings.reset_combo;

    uint64_t now = bflb_mtimer_get_time_ms();
    if (held != combo_held) {
        combo_held = held;
        combo_since = now;
        return false;
    }
    if (!held || now - combo_since < COMBO_HOLD_MS)
        return false;

    wait_combo_release(held);
    pending_action = held == settings.menu_combo ? ACTION_MENU : ACTION_RESET;
    dprint("Combo %04x: action %d", held, pending_action);
    return true;
}

// MODE is wired to the FPGA's RECONFIG_N, so firmware can't see the button.
// What it can see: while MODE is held the FPGA is unconfigured and doesn't
// answer, and after release the flash bitstream answers with a different
// core ID. The silent time is roughly hold time + reload time.
static bool check_mode_button() {
    uint64_t now = bflb_mtimer_get_time_ms();
    if (now - last_poll < CORE_POLL_MS)
        return false;
    last_poll = now;

    int16_t id = get_core_id();         // up to 200ms when the FPGA is silent
    now = bflb_mtimer_get_time_ms();
    if (id < 0) {
        if (!silent_since) silent_since = now;
        return false;
    }
    if (id == watch_core) {             // still our game (a dropped reply)
        silent_since = 0;
        return false;
    }

    // a different core answered: the FPGA was reconfigured underneath us
    uint32_t silent_ms = silent_since ? (uint32_t)(now - silent_since) : 0;
    uint32_t threshold = settings.mode_reload_ms + settings.mode_hold_ms;
    threshold = threshold > CORE_POLL_MS ? threshold - CORE_POLL_MS : 0;
    pending_action = silent_ms >= threshold ? ACTION_MENU : ACTION_RELOAD;
    dprint("FPGA reconfigured: core %d -> %d after %lu ms silent, action %d",
           watch_core, id, (unsigned long)silent_ms, pending_action);
    silent_since = 0;
    return true;
}

bool game_watch_poll(uint16_t pad1, uint16_t pad2) {
    if (pending_action != ACTION_NONE)
        return true;
    if (check_combos(pad1, pad2))
        return true;
    if (watch_core > 0 && check_mode_button())
        return true;
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
    overlay_printf("  for 1 second.");
    overlay_cursor(0, 15);
    overlay_printf("  Wait 10s to cancel.");

    // let go of the button that opened this screen
    wait_combo_release(0xfff);

    uint64_t start = bflb_mtimer_get_time_ms();
    uint64_t steady_since = 0;
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
            overlay_cursor(0, 17);
            overlay_printf("  %-28s", cur ? combo_to_string(cur, true).c_str() : "");
        }
        if (cur)
            start = now;            // any input restarts the cancel timeout
        if (combo_count(cur) == COMBO_BUTTONS && now - steady_since >= 1000) {
            wait_combo_release(0xfff);
            return cur;
        }
        if (now - start >= 10000)
            return 0;
        delay(20);
    }
}

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
        overlay_printf("Menu:  %s", combo_to_string(edit.menu_combo, true).c_str());
        overlay_cursor(2, 11);
        overlay_printf("Reset: %s", combo_to_string(edit.reset_combo, true).c_str());
        overlay_cursor(2, 12);
        overlay_printf("MODE hold: %lu.%lu s", (unsigned long)(edit.mode_hold_ms / 1000),
                       (unsigned long)(edit.mode_hold_ms % 1000 / 100));
        overlay_cursor(2, 14);
        overlay_printf("Save");
        overlay_cursor(2, 15);
        overlay_printf("<< Back");
        overlay_cursor(2, 18);
        overlay_printf("In game, hold a combo to use it.");
        overlay_cursor(2, 19);
        overlay_printf("MODE: tap = reset, hold = menu");
        if (!message.empty()) {
            overlay_cursor(2, 21);
            overlay_printf("%s", message.c_str());
        }
    }

    std::vector<int> get_options() override {
        return {10, 11, 12, 14, 15};
    }

    bool set_combo(uint16_t *target, uint16_t other, const char *what) {
        uint16_t c = capture_combo(what);
        const char *why = NULL;
        if (!c)
            message = "Cancelled";
        else if (!combo_valid(c, &why))
            message = why;
        else if (c == other)
            message = "Already used by the other combo";
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
            return set_combo(&edit.menu_combo, edit.reset_combo, "menu combo");
        case 1:
            return set_combo(&edit.reset_combo, edit.menu_combo, "reset combo");
        case 2:     // cycle 2s .. 5s
            edit.mode_hold_ms = edit.mode_hold_ms >= 5000 ? 2000 : (edit.mode_hold_ms / 1000 + 1) * 1000;
            message = "";
            do_redraw();
            wait_combo_release(BTN_A | BTN_B);
            return false;
        case 3:
            settings = edit;
            message = settings_save() ? "Saved" : "Save failed. Read-only drive?";
            do_redraw();
            wait_combo_release(BTN_A | BTN_B);
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
    delay(300);             // let go of the button that opened Options
    menu_input_loop();
    menu_clear();
}
