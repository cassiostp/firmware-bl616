#include <string>
#include <memory>

extern "C" {
#include "bl616_glb.h"
#include "bl616_hbn.h"
#include "bflb_irq.h"
#include "bflb_wdg.h"
}

#include "game_controls.h"
#include "settings.h"
#include "menu_manager.h"
#include "overlay.h"
#include "utils.h"

// Before a deliberate restart: the watchdog mustn't fire in whatever runs
// next (the ROM flashing loader in flash mode).
static void stop_watchdog(void) {
    if (wdg_dev)
        bflb_wdg_stop(wdg_dev);
}

static void restart_mcu(bool flash_mode);

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
// A game core that hasn't answered yet (it was just programmed) can't show
// that silence, but it never answers as the flash bitstream: core 0 there
// means MODE was pressed before the core's first answer.
static void check_mode_button(bool in_game, bool game_loaded) {
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
    if (!seen_answer && id == 0 && game_loaded)
        reloaded = true;
    if (reloaded) {
        dprint("FPGA back as core %d: MODE pressed, restarting", id);
        overlay(1);
        overlay_status("Restarting...");
        delay(50);
        restart_mcu(false);
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
    check_mode_button(in_game, game_loaded);
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
#ifndef TANGCORE_HOST               // the host sim has no SoC registers
    volatile uint32_t *r = (volatile uint32_t *)addr;
    *r = on ? (*r | bits) : (*r & ~bits);
#endif
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
// power cut. Verified on a Console 138K: the ROM shows up as "Bouffalo CDC"
// on the PC plugged into the BL616 port.
static void reboot_to_flash_mode(void) {
    overlay_clear();
    overlay_cursor(0, 9);
    //              01234567890123456789012345678901
    overlay_printf("  --- Flash mode ---");
    overlay_cursor(0, 11);
    overlay_printf("  Ready to be flashed from the");
    overlay_cursor(0, 12);
    overlay_printf("  PC on the BL616 USB-C port.");
    overlay_cursor(0, 14);
    overlay_printf("  Keep the power connected.");
    overlay_cursor(0, 15);
    overlay_printf("  Power-cycle to cancel.");
    delay(100);                     // let the UART drain
    restart_mcu(true);
}

// Restart the MCU like a power cycle, or (flash_mode) into the ROM's USB
// loader. Both need the USB block back in its power-on state first: after a
// software reset Sipeed's bootloader finds the port still set up as a host,
// takes it for a PC and stays in its debug mode, so TangCore never starts
// (the screen keeps whatever the FPGA last showed). The ROM's loader likewise
// never shows up on the PC.
static void restart_mcu(bool flash_mode) {
    taskENTER_CRITICAL();
    usb_back_to_power_on_state();
    arch_delay_ms(100);             // long enough for the other end to see a detach
    if (flash_mode)
        HBN_Set_User_Boot_Config(1);    // 1: boot from interface (download mode)
    stop_watchdog();
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
        overlay_printf("<< Cancel");
    }

    std::vector<int> get_options() override {
        return {17, 18};
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
        overlay_printf("Pause in game menu: %s", edit.pause_in_menu ? "ON" : "OFF");
        overlay_cursor(2, 15);
        overlay_printf("Flash mode...");
        overlay_cursor(2, 16);
        overlay_printf("Save");
        overlay_cursor(2, 17);
        overlay_printf("<< Back");
        overlay_cursor(2, 20);
        //                01234567890123456789012345678901
        overlay_printf("In game:");
        overlay_cursor(2, 21);
        overlay_printf(" Menu combo: game menu");
        overlay_cursor(2, 22);
        overlay_printf(" Reset combo: reset the game,");
        overlay_cursor(2, 23);
        overlay_printf(" keep holding: close the game");
        overlay_cursor(2, 24);
        overlay_printf("Video: in the game menu");
        if (!message.empty()) {
            overlay_cursor(2, 26);
            overlay_printf("%s", message.c_str());
        }
    }

    std::vector<int> get_options() override {
        return {9, 10, 11, 12, 13, 14, 15, 16, 17};
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
            edit.pause_in_menu = !edit.pause_in_menu;
            break;
        case 6:
            push_menu(std::unique_ptr<Menu>(new FlashModeMenu()));
            return false;
        case 7:
            settings = edit;
            message = settings_save() ? "Saved" : "Save failed. Read-only drive?";
            apply_core_config();    // the running core gets them right away
            break;
        default:
            return true;    // back
        }
        do_redraw();
        return false;
    }
};

/////////////////////////////////////////////////////////////////////////////////
// Scanlines, in the game menu: changes reach the running core at once, and
// are saved to tangcore.cfg on the way out

// Show the game without the menu, and change the scanlines on it. Returns
// true if anything changed. The game stays paused, except on the SNES and
// MegaDrive: they buffer only a few lines, so a paused game shows garbage
// without the menu. There it runs, with the pads muted.
extern int16_t active_core;

static bool scanline_preview(void) {
    bool changed = false;
    suppress_held_buttons();            // the A that chose Preview
    bool live = active_core == 2 || active_core == 4;
    core_config_preview(live ? PREVIEW_LIVE : PREVIEW_PAUSED);
    overlay(0);
    for (;;) {
        uint16_t joy1, joy2, hid1, hid2;
        get_joypad_states(&joy1, &joy2, &hid1, &hid2);
        uint16_t p = joy1 | hid1 | joy2 | hid2;
        if (controls_poll(joy1 | hid1, joy2 | hid2, false, game_loaded()))
            break;                      // the menu combo: back to the game
        bool step = true;
        if ((p & BTN_LEFT) && settings.scanline_dark > 0)
            settings.scanline_dark--;
        else if ((p & BTN_RIGHT) && settings.scanline_dark < 3)
            settings.scanline_dark++;
        else if (p & (BTN_UP | BTN_DOWN))
            settings.scanline_thick = !settings.scanline_thick;
        else if (p & BTN_SELECT)
            settings.scanlines = !settings.scanlines;
        else if (p & BTN_START)
            settings.scanline_full = !settings.scanline_full;
        else if (p & (BTN_A | BTN_B))
            break;
        else
            step = false;
        if (step) {
            settings.scanlines |= !(p & BTN_SELECT);    // adjusting lines turns them on
            apply_core_config();
            changed = true;
            suppress_held_buttons();    // one press, one step
        }
        delay(20);
    }
    suppress_held_buttons();            // the button that ended it isn't a menu choice
    overlay(1);
    core_config_preview(PREVIEW_OFF);
    return changed;
}

struct ScanlineMenu: Menu {
    bool changed = false;
    std::string message;

    ~ScanlineMenu() {                   // left some other way than Back (the menu combo)
        if (changed)
            settings_save();
    }

    void render() override {
        overlay_clear();
        overlay_cursor(0, 7);
        //              01234567890123456789012345678901
        overlay_printf("  --- Scanlines ---");
        overlay_cursor(2, 9);
        overlay_printf("Scanlines: %s", settings.scanlines ? "ON" : "OFF");
        overlay_cursor(2, 10);
        overlay_printf("Darkness: %d%%", scanline_dark_percent(settings.scanline_dark));
        overlay_cursor(2, 11);
        overlay_printf("Lines: %s", settings.scanline_thick ? "Thick" : "Thin");
        overlay_cursor(2, 12);
        overlay_printf("Scale: %s", settings.scanline_full ? "Full" : "Integer");
        overlay_cursor(2, 13);
        overlay_printf("Preview");
        overlay_cursor(2, 15);
        overlay_printf("<< Back");
        overlay_cursor(2, 17);
        //                01234567890123456789012345678901
        overlay_printf("Integer: even lines, smaller");
        overlay_cursor(2, 18);
        overlay_printf("picture. Full: full size.");
        overlay_cursor(2, 20);
        overlay_printf("Preview hides this menu:");
        overlay_cursor(2, 21);
        overlay_printf(" LEFT/RIGHT  darkness");
        overlay_cursor(2, 22);
        overlay_printf(" UP/DOWN     thin/thick");
        overlay_cursor(2, 23);
        overlay_printf(" START       scale");
        overlay_cursor(2, 24);
        overlay_printf(" SELECT      on/off");
        overlay_cursor(2, 25);
        overlay_printf(" A or B      back here");
        if (!message.empty()) {
            overlay_cursor(2, 27);
            overlay_printf("%s", message.c_str());
        }
    }

    std::vector<int> get_options() override {
        return {9, 10, 11, 12, 13, 15};
    }

    bool on_choose(int idx) override {
        message = "";
        switch (idx) {
        case 0:
            settings.scanlines = !settings.scanlines;
            break;
        case 1:
            settings.scanline_dark = (settings.scanline_dark + 1) & 3;
            settings.scanlines = true;
            break;
        case 2:
            settings.scanline_thick = !settings.scanline_thick;
            settings.scanlines = true;
            break;
        case 3:
            settings.scanline_full = !settings.scanline_full;
            settings.scanlines = true;
            break;
        case 4:
            changed |= scanline_preview();
            do_redraw();
            return false;
        default:                        // << Back: keep what was chosen
            if (changed && !settings_save()) {
                changed = false;        // Back again leaves without saving
                message = "Save failed. Read-only drive?";
                do_redraw();
                return false;
            }
            changed = false;
            return true;
        }
        changed = true;
        apply_core_config();
        do_redraw();
        return false;
    }
};

Menu *create_scanline_menu(void) {
    return new ScanlineMenu();
}

////////////////////////////////////////////////////////////////////////////////
// Video filters, in the game menu: changes reach the running core at once
// (frame 0x13, next to core_config), and are saved to tangcore.cfg on the
// way out

// The LCD grid needs an integer scale in both directions: only the handheld
// cores offer it (GBA, and SMS in Game Gear mode: core_config bit 0, set by
// the .gg loader).
static bool lcd_grid_offered(void) {
    return active_core == 3 || (active_core == 5 && (get_core_config() & 1u));
}

// Show the game without the menu while the pads adjust a filter: step()
// applies one press and returns true if a setting changed. Returns true if
// anything changed. Paused, except on SNES/MD (live, pads muted): same as
// the scanline preview.
static bool video_preview(bool (*step)(uint16_t p)) {
    suppress_held_buttons();            // the A that chose Preview
    bool live = active_core == 2 || active_core == 4;
    core_config_preview(live ? PREVIEW_LIVE : PREVIEW_PAUSED);
    overlay(0);
    bool changed = false;
    for (;;) {
        uint16_t joy1, joy2, hid1, hid2;
        get_joypad_states(&joy1, &joy2, &hid1, &hid2);
        uint16_t p = joy1 | hid1 | joy2 | hid2;
        if (controls_poll(joy1 | hid1, joy2 | hid2, false, game_loaded()))
            break;                      // the menu combo: back to the game
        if (p & (BTN_A | BTN_B))
            break;
        if (step(p)) {
            apply_core_config();
            changed = true;
            suppress_held_buttons();    // one press, one step
        }
        delay(20);
    }
    suppress_held_buttons();            // the button that ended it isn't a menu choice
    overlay(1);
    core_config_preview(PREVIEW_OFF);
    return changed;
}

// Cycle a -4..+3 field up by one.
static int8_t cycle_pm3(int8_t v) {
    return (int8_t)(((v + 4 + 1) & 7) - 4);
}

static bool color_preview_step(uint16_t p) {
    if ((p & BTN_LEFT) && settings.video_brightness > -4)
        settings.video_brightness--;
    else if ((p & BTN_RIGHT) && settings.video_brightness < 3)
        settings.video_brightness++;
    else if ((p & BTN_UP) && settings.video_contrast < 3)
        settings.video_contrast++;
    else if ((p & BTN_DOWN) && settings.video_contrast > -4)
        settings.video_contrast--;
    else if (p & BTN_SELECT)
        settings.video_saturation = cycle_pm3(settings.video_saturation);
    else if (p & BTN_START)
        settings.video_gamma = (settings.video_gamma + 1) & 3;
    else
        return false;
    return true;
}

static bool mask_preview_step(uint16_t p) {
    if (p & BTN_LEFT)
        settings.crt_mask_strength = (settings.crt_mask_strength + 3) & 3;
    else if (p & BTN_RIGHT)
        settings.crt_mask_strength = (settings.crt_mask_strength + 1) & 3;
    else if (p & (BTN_UP | BTN_DOWN))
        settings.crt_mask = (settings.crt_mask + 1) & 3;
    else
        return false;
    return true;
}

static bool grid_preview_step(uint16_t p) {
    if (p & BTN_LEFT)
        settings.lcd_grid_strength = (settings.lcd_grid_strength + 3) & 3;
    else if (p & BTN_RIGHT)
        settings.lcd_grid_strength = (settings.lcd_grid_strength + 1) & 3;
    else if (p & (BTN_UP | BTN_DOWN))
        settings.lcd_grid = !settings.lcd_grid;
    else
        return false;
    return true;
}

static const char *gamma_name(uint8_t g) {
    static const char *names[] = {"Off", "Darker", "Brighter", "CRT"};
    return names[g & 3];
}

static const char *mask_name(uint8_t m) {
    static const char *names[] = {"Off", "Grille", "Slot", "Dot"};
    return names[m & 3];
}

struct ColorMenu: Menu {
    bool changed = false;
    std::string message;

    ~ColorMenu() {
        if (changed)
            settings_save();
    }

    void render() override {
        overlay_clear();
        overlay_cursor(0, 7);
        //              01234567890123456789012345678901
        overlay_printf("  --- Color ---");
        overlay_cursor(2, 9);
        overlay_printf("Brightness: %d", settings.video_brightness);
        overlay_cursor(2, 10);
        overlay_printf("Contrast: %d", settings.video_contrast);
        overlay_cursor(2, 11);
        overlay_printf("Saturation: %d", settings.video_saturation);
        overlay_cursor(2, 12);
        overlay_printf("Gamma: %s", gamma_name(settings.video_gamma));
        overlay_cursor(2, 13);
        overlay_printf("Reset colors");
        overlay_cursor(2, 14);
        overlay_printf("Preview");
        overlay_cursor(2, 16);
        overlay_printf("<< Back");
        overlay_cursor(2, 18);
        overlay_printf("Preview hides this menu:");
        overlay_cursor(2, 19);
        overlay_printf(" LEFT/RIGHT  brightness");
        overlay_cursor(2, 20);
        overlay_printf(" UP/DOWN     contrast");
        overlay_cursor(2, 21);
        overlay_printf(" SELECT      saturation");
        overlay_cursor(2, 22);
        overlay_printf(" START       gamma");
        overlay_cursor(2, 23);
        overlay_printf(" A or B      back here");
        if (!message.empty()) {
            overlay_cursor(2, 25);
            overlay_printf("%s", message.c_str());
        }
    }

    std::vector<int> get_options() override {
        return {9, 10, 11, 12, 13, 14, 16};
    }

    bool on_choose(int idx) override {
        message = "";
        switch (idx) {
        case 0:
            settings.video_brightness = cycle_pm3(settings.video_brightness);
            break;
        case 1:
            settings.video_contrast = cycle_pm3(settings.video_contrast);
            break;
        case 2:
            settings.video_saturation = cycle_pm3(settings.video_saturation);
            break;
        case 3:
            settings.video_gamma = (settings.video_gamma + 1) & 3;
            break;
        case 4:
            settings.video_brightness = 0;
            settings.video_contrast = 0;
            settings.video_saturation = 0;
            settings.video_gamma = 0;
            break;
        case 5:
            changed |= video_preview(color_preview_step);
            do_redraw();
            return false;
        default:                        // << Back: keep what was chosen
            if (changed && !settings_save()) {
                changed = false;        // Back again leaves without saving
                message = "Save failed. Read-only drive?";
                do_redraw();
                return false;
            }
            changed = false;
            return true;
        }
        changed = true;
        apply_core_config();
        do_redraw();
        return false;
    }
};

struct CrtMaskMenu: Menu {
    bool changed = false;
    std::string message;

    ~CrtMaskMenu() {
        if (changed)
            settings_save();
    }

    void render() override {
        overlay_clear();
        overlay_cursor(0, 7);
        //              01234567890123456789012345678901
        overlay_printf("  --- CRT mask ---");
        overlay_cursor(2, 9);
        overlay_printf("Mask: %s", mask_name(settings.crt_mask));
        overlay_cursor(2, 10);
        overlay_printf("Strength: %d", (settings.crt_mask_strength & 3) + 1);
        overlay_cursor(2, 11);
        overlay_printf("Preview");
        overlay_cursor(2, 13);
        overlay_printf("<< Back");
        overlay_cursor(2, 15);
        overlay_printf("Preview hides this menu:");
        overlay_cursor(2, 16);
        overlay_printf(" LEFT/RIGHT  strength");
        overlay_cursor(2, 17);
        overlay_printf(" UP/DOWN     mask type");
        overlay_cursor(2, 18);
        overlay_printf(" A or B      back here");
        if (!message.empty()) {
            overlay_cursor(2, 20);
            overlay_printf("%s", message.c_str());
        }
    }

    std::vector<int> get_options() override {
        return {9, 10, 11, 13};
    }

    bool on_choose(int idx) override {
        message = "";
        switch (idx) {
        case 0:
            settings.crt_mask = (settings.crt_mask + 1) & 3;
            break;
        case 1:
            settings.crt_mask_strength = (settings.crt_mask_strength + 1) & 3;
            break;
        case 2:
            changed |= video_preview(mask_preview_step);
            do_redraw();
            return false;
        default:                        // << Back: keep what was chosen
            if (changed && !settings_save()) {
                changed = false;        // Back again leaves without saving
                message = "Save failed. Read-only drive?";
                do_redraw();
                return false;
            }
            changed = false;
            return true;
        }
        changed = true;
        apply_core_config();
        do_redraw();
        return false;
    }
};

struct LcdGridMenu: Menu {
    bool changed = false;
    std::string message;

    ~LcdGridMenu() {
        if (changed)
            settings_save();
    }

    void render() override {
        overlay_clear();
        overlay_cursor(0, 7);
        //              01234567890123456789012345678901
        overlay_printf("  --- LCD grid ---");
        overlay_cursor(2, 9);
        overlay_printf("Grid: %s", settings.lcd_grid ? "ON" : "OFF");
        overlay_cursor(2, 10);
        overlay_printf("Strength: %d", (settings.lcd_grid_strength & 3) + 1);
        overlay_cursor(2, 11);
        overlay_printf("Preview");
        overlay_cursor(2, 13);
        overlay_printf("<< Back");
        overlay_cursor(2, 15);
        overlay_printf("Preview hides this menu:");
        overlay_cursor(2, 16);
        overlay_printf(" LEFT/RIGHT  strength");
        overlay_cursor(2, 17);
        overlay_printf(" UP/DOWN     on/off");
        overlay_cursor(2, 18);
        overlay_printf(" A or B      back here");
        if (!message.empty()) {
            overlay_cursor(2, 20);
            overlay_printf("%s", message.c_str());
        }
    }

    std::vector<int> get_options() override {
        return {9, 10, 11, 13};
    }

    bool on_choose(int idx) override {
        message = "";
        switch (idx) {
        case 0:
            settings.lcd_grid = !settings.lcd_grid;
            break;
        case 1:
            settings.lcd_grid_strength = (settings.lcd_grid_strength + 1) & 3;
            break;
        case 2:
            changed |= video_preview(grid_preview_step);
            do_redraw();
            return false;
        default:                        // << Back: keep what was chosen
            if (changed && !settings_save()) {
                changed = false;        // Back again leaves without saving
                message = "Save failed. Read-only drive?";
                do_redraw();
                return false;
            }
            changed = false;
            return true;
        }
        changed = true;
        apply_core_config();
        do_redraw();
        return false;
    }
};

struct VideoMenu: Menu {
    std::vector<int> rows;

    VideoMenu() {
        rows = lcd_grid_offered() ? std::vector<int>{9, 10, 11, 12, 14}
                                  : std::vector<int>{9, 10, 11, 14};
    }

    void render() override {
        overlay_clear();
        overlay_cursor(0, 7);
        //              01234567890123456789012345678901
        overlay_printf("  --- Video ---");
        overlay_cursor(2, 9);
        overlay_printf("Scanlines...");
        overlay_cursor(2, 10);
        overlay_printf("Color...");
        overlay_cursor(2, 11);
        overlay_printf("CRT mask...");
        if (lcd_grid_offered()) {
            overlay_cursor(2, 12);
            overlay_printf("LCD grid...");
        }
        overlay_cursor(2, 14);
        overlay_printf("<< Back");
    }

    std::vector<int> get_options() override {
        return rows;
    }

    bool on_choose(int idx) override {
        int row = rows[idx];
        if (row == 9) {
            push_menu(std::unique_ptr<Menu>(new ScanlineMenu()));
            return false;
        } else if (row == 10) {
            push_menu(std::unique_ptr<Menu>(new ColorMenu()));
            return false;
        } else if (row == 11) {
            push_menu(std::unique_ptr<Menu>(new CrtMaskMenu()));
            return false;
        } else if (row == 12) {
            push_menu(std::unique_ptr<Menu>(new LcdGridMenu()));
            return false;
        }
        return true;                    // << Back: the game menu stays loaded
    }
};

Menu *create_video_menu(void) {
    return new VideoMenu();
}

void menu_options(void) {
    menu_clear();
    push_menu(std::unique_ptr<Menu>(new OptionsMenu()));
    menu_current()->do_redraw();
    suppress_held_buttons();    // the button that opened Options
    menu_input_loop();
    menu_clear();
}
