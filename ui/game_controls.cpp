#include <string>
#include <memory>

extern "C" {
#include "bl616_glb.h"
#include "bl616_hbn.h"
#include "bflb_irq.h"
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
#define RESET_HOLD_MS     250     // reset combo: held this long resets the game
#define POLL_MS_GAME      250     // how often to ask the FPGA for its core ID
#define POLL_MS_MENU      500
#define POLL_MS_SILENT_CORE 2000  // a core that never answered: don't stall input
#define MODE_SILENT_MS    600     // FPGA silent this long, then back: MODE was pressed
#define MODE_MISSES       3       // ...and at least this many polls in a row unanswered
#define POLL_GAP_MS       1500    // polls further apart than this don't count as in a row

static uint16_t combo_held;         // which combo is being held (0 = none)
static uint64_t combo_since;        // when it started being held
static bool reset_fired;            // the held reset combo already reset the game
static uint64_t last_poll;
static bool seen_answer;            // the FPGA answered since the last reset
static int16_t last_id;             // the last core ID it answered
static int misses;                  // polls in a row without an answer
static uint64_t silent_since;       // start of the first unanswered poll (0 = answering)

void controls_reset(void) {
    combo_held = 0;
    combo_since = 0;
    reset_fired = false;
    last_poll = bflb_mtimer_get_time_ms();
    seen_answer = false;
    last_id = -1;
    misses = 0;
    silent_since = 0;
}

static bool pressed(uint16_t pad1, uint16_t pad2, uint16_t combo) {
    return combo && (((pad1 & combo) == combo) || ((pad2 & combo) == combo));
}

static uint16_t active_reset_combo(void) {
    return settings.reset_enabled ? settings.reset_combo : 0;
}

bool combo_in_progress(uint16_t pad1, uint16_t pad2) {
    uint16_t combos[] = {settings.menu_combo, active_reset_combo()};
    for (uint16_t c : combos)
        if (c && (combo_count(pad1 & c) >= 2 || combo_count(pad2 & c) >= 2))
            return true;
    return false;
}

static bool check_combos(uint16_t pad1, uint16_t pad2, bool in_game, bool game_loaded) {
    uint16_t held = 0;
    if (pressed(pad1, pad2, settings.menu_combo))
        held = settings.menu_combo;
    else if (pressed(pad1, pad2, active_reset_combo()))
        held = active_reset_combo();

    uint64_t now = bflb_mtimer_get_time_ms();
    if (held != combo_held) {
        combo_held = held;
        combo_since = now;
        reset_fired = false;
        return false;
    }
    if (!held || !game_loaded)
        return false;

    GameAction action = ACTION_NONE;
    bool done = true;               // the combo is used up: ignore it until released
    if (held == settings.menu_combo) {
        if (now - combo_since >= MENU_HOLD_MS)
            action = in_game ? ACTION_GAME_MENU : ACTION_RESUME;
    } else if (in_game) {           // reset combo: reset soon, close if held on
        if (now - combo_since >= settings.close_hold_ms) {
            action = ACTION_CLOSE;
        } else if (!reset_fired && now - combo_since >= RESET_HOLD_MS) {
            action = ACTION_RESET;
            reset_fired = true;
            done = false;           // keep timing the hold
        }
    }
    if (action == ACTION_NONE)
        return false;

    if (done) {
        // keep the buttons from reaching menus or the core until let go
        suppress_held_buttons();
        combo_held = 0;
    }
    pending_action = action;
    dprint("Combo %04x: action %d", held, action);
    return true;
}

// MODE is wired to the FPGA's RECONFIG_N, so firmware can't see the button.
// What it can see: the FPGA stops answering while it reloads from flash, then
// answers again, as the flash bitstream (core ID 0). Single replies get
// dropped (while the FPGA sends a joypad or disk frame), so it takes several
// misses in a row, over a real stretch of time. MODE then restarts
// everything, like a power cycle.
static void check_mode_button(bool in_game) {
    uint32_t interval = !seen_answer ? POLL_MS_SILENT_CORE : in_game ? POLL_MS_GAME : POLL_MS_MENU;
    uint64_t poll_start = bflb_mtimer_get_time_ms();
    uint64_t gap = poll_start - last_poll;
    if (gap < interval)
        return;
    last_poll = poll_start;
    if (gap > POLL_GAP_MS) {        // we weren't watching (ROM load, dialog...): start over
        misses = 0;
        silent_since = 0;
    }

    int16_t id = get_core_id();     // up to 200ms when the FPGA is silent
    if (id < 0) {
        if (seen_answer) {
            if (!misses)
                silent_since = poll_start;
            misses++;
        }
        return;
    }
    uint64_t now = bflb_mtimer_get_time_ms();
    bool reloaded = misses >= MODE_MISSES && now - silent_since >= MODE_SILENT_MS;
    // a game core that comes back as itself only dropped replies (e.g. the
    // NES core resets its serial link on Select+Down)
    if (reloaded && last_id > 0 && id == last_id)
        reloaded = false;
    if (reloaded) {
        dprint("FPGA silent for %lu ms, back as core %d: MODE pressed, restarting",
               (unsigned long)(now - silent_since), id);
        overlay(1);
        overlay_status("Restarting...");
        delay(50);
        GLB_SW_System_Reset();
    }
    seen_answer = true;
    last_id = id;
    misses = 0;
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

// BL616 USB registers (drivers/lhal/src/bflb_usb_v2.c in the SDK)
#define TC_USB_BASE                0x20072000
#define USB_OTG_CSR             (TC_USB_BASE + 0x80)
#define USB_A_BUS_REQ_HOV       (1 << 4)
#define USB_A_BUS_DROP_HOV      (1 << 5)
#define USB_PHY_TST             (TC_USB_BASE + 0x114)
#define USB_UNPLUG              (1 << 0)
#define PDS_USB_CTL             0x2000e500
#define PDS_USB_SW_RST_N        (1 << 0)
#define PDS_USB_EXT_SUSP_N      (1 << 1)
#define PDS_USB_IDDIG           (1 << 5)
#define PDS_USB_PHY_CTRL        0x2000e504
#define PDS_USB_PHY_PONRST      (1 << 0)
#define PDS_USB_PU_USB20_PSW    (1 << 6)
#define USB_IRQ                 37

static inline void reg_set(uint32_t addr, uint32_t bits, bool on) {
    volatile uint32_t *r = (volatile uint32_t *)addr;
    *r = on ? (*r | bits) : (*r & ~bits);
}

// Put the USB block back the way power-on leaves it. The firmware runs it as a
// host (device-A, bus powered, PHY on), and these registers sit in the PDS
// power domain, which a software reset doesn't clear. The ROM's USB loader
// expects power-on state, so without this it never shows up on the PC.
static void usb_back_to_power_on_state(void) {
    bflb_irq_disable(USB_IRQ);
    reg_set(USB_OTG_CSR, USB_A_BUS_REQ_HOV, false);     // stop driving the bus
    reg_set(USB_OTG_CSR, USB_A_BUS_DROP_HOV, true);
    reg_set(USB_PHY_TST, USB_UNPLUG, true);             // detach (as usb_dc_deinit)
    reg_set(PDS_USB_CTL, PDS_USB_IDDIG, true);          // B-device, not host
    reg_set(PDS_USB_CTL, PDS_USB_SW_RST_N, false);      // hold the controller in reset
    reg_set(PDS_USB_CTL, PDS_USB_EXT_SUSP_N, false);
    reg_set(PDS_USB_PHY_CTRL, PDS_USB_PHY_PONRST, false);   // PHY off
    reg_set(PDS_USB_PHY_CTRL, PDS_USB_PU_USB20_PSW, false);
}

// Reboot the BL616 into its ROM bootloader, as if BOOT were held at power-up,
// so the firmware can be flashed without opening the case. The ROM reads the
// boot selection from HBN_RSV2, which survives a software reset but not a
// power cut. por: a power-on reset instead of a system reset (resets more of
// the chip; not yet known whether HBN_RSV2 survives it).
static void reboot_to_flash_mode(bool por) {
    overlay_clear();
    overlay_cursor(0, 9);
    //              01234567890123456789012345678901
    overlay_printf("  --- Flash mode %s---", por ? "(B) " : "");
    overlay_cursor(0, 11);
    overlay_printf("  Ready to be flashed from the");
    overlay_cursor(0, 12);
    overlay_printf("  PC on the BL616 USB-C port.");
    overlay_cursor(0, 14);
    overlay_printf("  Keep the power connected.");
    overlay_cursor(0, 15);
    overlay_printf("  Power-cycle to cancel.");
    delay(100);                     // let the UART drain
    taskENTER_CRITICAL();
    usb_back_to_power_on_state();
    arch_delay_ms(100);             // long enough for the PC to see a detach
    HBN_Set_User_Boot_Config(1);    // 1: boot from interface (download mode)
    if (por)
        GLB_SW_POR_Reset();
    else
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
        overlay_printf("  flashed, without the BOOT");
        overlay_cursor(0, 13);
        overlay_printf("  button. Plug the PC into the");
        overlay_cursor(0, 14);
        overlay_printf("  BL616 USB-C port first, and");
        overlay_cursor(0, 15);
        overlay_printf("  keep the power connected.");
        overlay_cursor(2, 17);
        overlay_printf("Restart in flash mode");
        overlay_cursor(2, 18);
        overlay_printf("Restart in flash mode (B)");
        overlay_cursor(2, 19);
        overlay_printf("<< Cancel");
    }

    std::vector<int> get_options() override {
        return {17, 18, 19};
    }

    bool on_choose(int idx) override {
        if (idx == 0 || idx == 1)
            reboot_to_flash_mode(idx == 1);     // doesn't return
        return true;
    }
};

struct OptionsMenu: Menu {
    Settings edit;
    std::string message;

    OptionsMenu() : edit(settings) {}

    void render() override {
        overlay_clear();
        overlay_cursor(0, 6);
        //              01234567890123456789012345678901
        overlay_printf("         --- Options ---");
        overlay_cursor(2, 9);
        overlay_printf("Menu:  %s", combo_to_string(edit.menu_combo, true).c_str());
        overlay_cursor(2, 10);
        overlay_printf("Reset: %s", combo_to_string(edit.reset_combo, true).c_str());
        overlay_cursor(2, 11);
        overlay_printf("Reset combo: %s", edit.reset_enabled ? "ON" : "OFF");
        overlay_cursor(2, 12);
        overlay_printf("Hold to close: %lu s", (unsigned long)(edit.close_hold_ms / 1000));
        overlay_cursor(2, 13);
        overlay_printf("Diagnostics: %s", edit.diag ? "ON" : "OFF");
        overlay_cursor(2, 14);
        overlay_printf("Flash mode...");
        overlay_cursor(2, 16);
        overlay_printf("Save");
        overlay_cursor(2, 17);
        overlay_printf("<< Back");
        overlay_cursor(2, 19);
        //                01234567890123456789012345678901
        overlay_printf("In game:");
        overlay_cursor(2, 20);
        overlay_printf(" Menu combo: game menu");
        overlay_cursor(2, 21);
        overlay_printf(" Reset combo: reset the game,");
        overlay_cursor(2, 22);
        overlay_printf(" keep holding: close the game");
        if (!message.empty()) {
            overlay_cursor(2, 24);
            overlay_printf("%s", message.c_str());
        }
    }

    std::vector<int> get_options() override {
        return {9, 10, 11, 12, 13, 14, 16, 17};
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
        message = "";
        switch (idx) {
        case 0:
            return set_combo(&edit.menu_combo, edit.reset_combo, "menu combo");
        case 1:
            return set_combo(&edit.reset_combo, edit.menu_combo, "reset combo");
        case 2:
            edit.reset_enabled = !edit.reset_enabled;
            break;
        case 3:                     // cycle 2s .. 5s
            edit.close_hold_ms = edit.close_hold_ms >= 5000 ? 2000 : (edit.close_hold_ms / 1000 + 1) * 1000;
            break;
        case 4:
            edit.diag = !edit.diag;
            break;
        case 5:
            push_menu(std::unique_ptr<Menu>(new FlashModeMenu()));
            return false;
        case 6:
            settings = edit;
            message = settings_save() ? "Saved" : "Save failed. Read-only drive?";
            break;
        default:
            return true;    // back
        }
        do_redraw();
        return false;
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
