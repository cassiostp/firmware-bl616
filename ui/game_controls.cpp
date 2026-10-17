#include <string>
#include <memory>
#include <stdio.h>

extern "C" {
#include "bl616_glb.h"
#include "bl616_hbn.h"
#include "bflb_irq.h"
#include "bflb_wdg.h"
}

#include "game_controls.h"
#include "settings.h"
#include "clock.h"
#include "cores.h"
#include "menu_manager.h"
#include "overlay.h"
#include "utils.h"
#include "dbg_log.h"
#include "dbg_console.h"
#include "usb_console.h"
#include "saves.h"
#include "uart_rx.h"

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
#define POLL_MS_MENU      50      // menus: three misses fit in a ~200ms reload
#define POLL_MS_SILENT_CORE 2000  // a core that never answered: don't stall input
#define MODE_MISSES       3       // ...and at least this many polls in a row unanswered
#define MODE_SILENT_MS    80      // ...spanning this long: one or two dropped replies aren't MODE
#define MODE_TIMEOUT_MENU 30      // menu polls wait this long: replies arrive in
                                  // well under 1ms, behind a 515-byte frame ~2.6ms
#define POLL_GAP_MS       1500    // polls further apart than this don't count as in a row
#define COMBO_GATE_MS     1500    // a combo state unchanged this long stops
                                  // blocking menu navigation (stuck pad bits)

static uint16_t combo_held;         // which combo is being held (0 = none)
static uint64_t combo_since;        // when it started being held
static bool reset_fired;            // the held reset combo already reset the game
static uint64_t last_poll;
static bool seen_answer;            // the FPGA answered since the last reset
static int16_t last_id;             // the last core ID it answered
static int misses;                  // polls in a row without an answer
static uint64_t silent_since;       // start of the first unanswered poll (0 = answering)
static uint64_t miss_log_at;        // last poll that logged a miss line (0 = none this streak)
static bool link_flushed;           // this silence already got an RX flush
static uint64_t link_flush_at;      // when it was sent
static uint32_t gate_sig;           // combo bits (both pads) doing the gating
static uint64_t gate_since;         // when that gating state last changed
static uint16_t mode_max_silence_ms;  // longest silent gap seen (diagnostic line)

// Longest silent gap (first miss to next answer) in ms, for the diagnostic
// line, so reload times can be reported. Stays 0 until a gap ends.
uint16_t mode_silence_ms(void) {
    return mode_max_silence_ms;
}

// Combo-gate edge logging (see combo_in_progress): when the effective
// gate last flipped, and whether the ">2 s held" line went out for it.
static bool gate_log_on;
static uint64_t gate_log_since;
static bool gate_held_logged;

void controls_reset(void) {
    combo_held = 0;
    combo_since = 0;
    reset_fired = false;
    last_poll = bflb_mtimer_get_time_ms();
    seen_answer = false;
    last_id = -1;
    misses = 0;
    silent_since = 0;
    miss_log_at = 0;
    mode_max_silence_ms = 0;
    gate_sig = 0;
    gate_since = 0;
    gate_log_on = false;
    gate_log_since = 0;
    gate_held_logged = false;
}

// A load-wait already saw the newly programmed core answer with this ID:
// record it, so the MODE detector doesn't take that first answer for a
// flash reload (a legitimately ID-0 bitstream like monitor.bin would
// otherwise restart the firmware once). The early-MODE rule still applies
// when nothing has answered yet (id < 0 primes nothing).
void controls_prime_core_id(int16_t id) {
    if (id < 0)
        return;
    seen_answer = true;
    last_id = id;
    misses = 0;
    silent_since = 0;
    miss_log_at = 0;
    last_poll = bflb_mtimer_get_time_ms();
}

static bool pressed(uint16_t pad1, uint16_t pad2, uint16_t combo) {
    return combo && (((pad1 & combo) == combo) || ((pad2 & combo) == combo));
}

static uint16_t active_reset_combo(void) {
    return settings.reset_enabled ? settings.reset_combo : 0;
}

bool combo_in_progress(uint16_t pad1, uint16_t pad2) {
    uint16_t combos[] = {settings.menu_combo, active_reset_combo()};
    uint16_t bits = settings.menu_combo | active_reset_combo();
    uint32_t sig = ((uint32_t)(pad1 & bits) << 16) | (pad2 & bits);
    bool gating = false;
    for (uint16_t c : combos)
        if (c && (combo_count(pad1 & c) >= 2 || combo_count(pad2 & c) >= 2))
            gating = true;
    uint64_t now = bflb_mtimer_get_time_ms();
    bool gated;
    if (!gating) {
        gate_sig = 0;
        gated = false;
    } else if (sig != gate_sig) {
        // Time-box the block: pads only report changes, so one lost release
        // frame (or a USB pad unplugged mid-press) holds combo bits forever.
        // A real combo completes within its short hold; a state that sits
        // unchanged far longer is stuck, and navigation must come back. Only
        // the combo bits count for "unchanged": other buttons still navigate.
        gate_sig = sig;
        gate_since = now;
        gated = true;
    } else {
        gated = now - gate_since < COMBO_GATE_MS;
    }
    // Log the effective gate's edges, not its level: navigation is dead
    // exactly while this is on, and the "off after ~1.5 s" line is the
    // time-box unwedging a stuck state.
    if (gated && !gate_log_on) {
        gate_log_on = true;
        gate_log_since = now;
        gate_held_logged = false;
        dbg_log("combo gate on: p=%03x/%03x", pad1 & 0xfff, pad2 & 0xfff);
    } else if (!gated && gate_log_on) {
        gate_log_on = false;
        dbg_log("combo gate off after %lums",
                (unsigned long)(now - gate_log_since));
    } else if (gated && !gate_held_logged && now - gate_log_since > 2000) {
        gate_held_logged = true;
        dbg_log("combo gate held >2s: p=%03x/%03x (stuck pads?)",
                pad1 & 0xfff, pad2 & 0xfff);
    }
    return gated;
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
        if (held)
            dbg_log("combo held %04x", held);
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
    dbg_log("combo %04x fired: action %d", held, (int)action);
    return true;
}

// MODE is wired to the FPGA's RECONFIG_N, so firmware can't see the button.
// What it can see: the FPGA stops answering while it reloads from flash, then
// answers again, as the flash bitstream (core ID 0). Single replies get
// dropped (while the FPGA sends a joypad frame), so it takes three misses in
// a row to count. In menus the polls run every 50ms with a 30ms timeout, so
// a 200-300ms reload shows several misses; in game (or whenever a game core
// answered last) core 0 after the game's ID is MODE at once, with no silence
// needed. MODE then restarts everything, like a power cycle.
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
        if (misses)
            dbg_log("mode: poll gap %lums, %d-miss streak discarded",
                    (unsigned long)gap, misses);
        misses = 0;
        silent_since = 0;
        link_flushed = false;
    }

    uint32_t timeout = in_game ? 200 : MODE_TIMEOUT_MENU;
    int16_t id = get_core_id_timeout(timeout);
    if (id < 0) {
        if (seen_answer) {
            if (!misses)
                silent_since = poll_start;
            misses++;
            // A dead link would log a line every 250 ms (thousands in one
            // stall): the first 3 misses, then one line every 10 s with the
            // count. The answer line below still summarises the streak.
            if (misses <= 3 || poll_start - miss_log_at >= 10000) {
                miss_log_at = poll_start;
                dbg_log("mode: miss %d (gap %lums, last=%d)",
                        misses, (unsigned long)gap, last_id);
            }
            // A game core silent for 1 s is not a reload (MODE answers as
            // core 0 within ~300 ms): its receiver may be counting down a
            // garbage frame length, swallowing these polls as parameters
            // (older bitstreams have no RX timeout). Complete that frame
            // harmlessly and say what this side's UART saw, so the log
            // distinguishes a desync (answers as itself right after the
            // flush) from an FPGA-side transmitter wedge (stays silent,
            // and a framing-error burst here would say the wire is bad).
            if (!link_flushed && game_loaded &&
                silent_since && poll_start - silent_since >= 1000) {
                link_flushed = true;
                link_flush_at = bflb_mtimer_get_time_ms();
                dbg_log("link: fpga silent %lums with a game loaded, flushing rx "
                        "(ringdrop %lu fifovf %d uarerr %lu intsts %08lx ring %lu)",
                        (unsigned long)(poll_start - silent_since),
                        (unsigned long)uart_rx_ring_dropped(),
                        uart_rx_fifo_overflowed() ? 1 : 0,
                        (unsigned long)uart_rx_err_count(),
                        (unsigned long)uart_rx_intsts(),
                        (unsigned long)uart_rx_ring_level());
                fpga_link_flush();
            }
        }
        return;
    }
    uint64_t now = bflb_mtimer_get_time_ms();
    uint64_t silent_ms = (seen_answer && misses && silent_since) ? now - silent_since : 0;
    if (silent_ms > mode_max_silence_ms)
        mode_max_silence_ms = silent_ms > 9999 ? 9999 : (uint16_t)silent_ms;
    bool reloaded = misses >= MODE_MISSES && silent_ms >= MODE_SILENT_MS;
    // a game core that comes back as itself only dropped replies (e.g. the
    // NES core resets its serial link on Select+Down)
    if (reloaded && last_id > 0 && id == last_id)
        reloaded = false;
    // a game core answering as the flash bitstream is MODE, however short
    // the silence (covers in-game reloads and menus with a game behind)
    if (seen_answer && last_id > 0 && id == 0)
        reloaded = true;
    if (!seen_answer && id == 0 && game_loaded)
        reloaded = true;
    if (misses)
        dbg_log("mode: answer id=%d last=%d seen=%d silent=%lums misses=%d%s",
                id, last_id, seen_answer ? 1 : 0, (unsigned long)silent_ms,
                misses, reloaded ? " -> MODE" : "");
    if (silent_ms >= 600)
        dbg_log("link: fpga silent %lums, back as core %d%s",
                (unsigned long)silent_ms, id, reloaded ? " (MODE)" : "");
    if (link_flushed) {
        bool same = !reloaded && id == last_id;
        dbg_log("link: fpga back %lums after flush as core %d "
                "(ringdrop %lu fifovf %d uarerr %lu intsts %08lx): %s",
                (unsigned long)(now - link_flush_at), id,
                (unsigned long)uart_rx_ring_dropped(),
                uart_rx_fifo_overflowed() ? 1 : 0,
                (unsigned long)uart_rx_err_count(),
                (unsigned long)uart_rx_intsts(),
                same ? "answered as itself, rx desync confirmed"
                     : "not the running core, flush not the cure");
        link_flushed = false;
        if (same)
            overlay(overlay_on());   // resend overlay state + core_config:
                                      // a garbage frame may have clobbered both
    }
    if (reloaded) {
        dprint("FPGA back as core %d after %lums silent: MODE pressed, restarting",
               id, (unsigned long)silent_ms);
        if (usb_console_enabled()) {
            // A reset here would kill the USB console and, with a PC on
            // the BL616 port, land in Sipeed's debug mode. Keep the MCU
            // (and the log's audience) alive: the main loop reprograms
            // the menu core and redraws instead.
            dbg_log("MODE with usb console on: reload without MCU reset");
            pending_action = ACTION_MODE_RELOAD;
        } else {
            overlay(1);
            overlay_status("Restarting...");
            delay(50);
            restart_mcu(false);
        }
    }
    seen_answer = true;
    last_id = id;
    misses = 0;
    silent_since = 0;
    miss_log_at = 0;
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
// steady for a second. Returns 0 if cancelled (10 seconds pass, from entry,
// without one). MODE is polled throughout, so it still recovers this screen.
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
    dbg_log("capture combo: %s start", what);

    uint64_t start = bflb_mtimer_get_time_ms();
    uint64_t steady_since = start;
    uint16_t last = 0;
    while (1) {
        uint64_t now = bflb_mtimer_get_time_ms();
        uint16_t joy1, joy2, hid1, hid2;
        get_joypad_states(&joy1, &joy2, &hid1, &hid2);
        check_mode_button(false, game_loaded());    // MODE restarts from here too
        uint16_t p1 = (joy1 | hid1) & 0xfff, p2 = (joy2 | hid2) & 0xfff;
        uint16_t cur = combo_count(p1) >= combo_count(p2) ? p1 : p2;

        if (cur != last) {
            last = cur;
            steady_since = now;
            overlay_cursor(0, 18);
            overlay_printf("  %-28s", cur ? combo_to_string(cur, true).c_str() : "");
        }
        // The cancel deadline is absolute from entry: restarting it on any
        // input let one stuck button hold this screen forever.
        if (combo_count(cur) == COMBO_BUTTONS && now - steady_since >= 1000) {
            overlay_cursor(0, 18);
            overlay_printf("  %-28s", (combo_to_string(cur, true) + " - got it").c_str());
            dbg_log("capture combo: %s got %04x after %lums",
                    what, cur, (unsigned long)(now - start));
            suppress_held_buttons();    // releasing them mustn't act on the menu
            delay(500);                 // long enough to read
            return cur;
        }
        if (now - start >= 10000) {
            dbg_log("capture combo: %s cancelled after %lums (held %04x)",
                    what, (unsigned long)(now - start), cur);
            return 0;
        }
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
    dbg_log("restart_mcu(flash=%d)", flash_mode ? 1 : 0);
    taskENTER_CRITICAL();
    usb_back_to_power_on_state();
    arch_delay_ms(100);             // long enough for the other end to see a detach
    if (flash_mode)
        HBN_Set_User_Boot_Config(1);    // 1: boot from interface (download mode)
    stop_watchdog();
    GLB_SW_System_Reset();
}

void tc_restart_mcu(bool flash_mode) {
    restart_mcu(flash_mode);
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

// Pad-driven date/time editor, like capture_combo: its own input loop, so
// Left/Right can adjust the field Up/Down selects. A saves (seconds restart
// at 0), B cancels. Returns true if the clock was set.
static bool edit_datetime(void) {
    static const char *wday_names[] = {"Sun", "Mon", "Tue", "Wed",
                                       "Thu", "Fri", "Sat"};
    DateTime dt;
    clock_now(dt);
    dt.sec = 0;
    int field = 0;                      // 0 year, 1 month, 2 day, 3 hour, 4 minute
    suppress_held_buttons();            // the button that opened this screen

    for (;;) {
        datetime_clamp(dt);
        overlay_clear();
        overlay_cursor(0, 7);
        //              01234567890123456789012345678901
        overlay_printf("  --- Date & time ---");
        overlay_cursor(2, 9);
        overlay_printf("%s %04d-%02d-%02d %02d:%02d", wday_names[dt.wday],
                       dt.year, dt.mon, dt.day, dt.hour, dt.min);
        static const char *names[] = {"Year", "Month", "Day", "Hour", "Minute"};
        int vals[] = {dt.year, dt.mon, dt.day, dt.hour, dt.min};
        for (int i = 0; i < 5; i++) {
            overlay_cursor(0, 11 + i);
            overlay_printf("%s", i == field ? ">" : " ");
            overlay_cursor(2, 11 + i);
            overlay_printf("%s: %d", names[i], vals[i]);
        }
        overlay_cursor(2, 18);
        overlay_printf("UP/DOWN field, LEFT/RIGHT set");
        overlay_cursor(2, 19);
        overlay_printf("A: save   B: cancel");
        overlay_cursor(2, 21);
        overlay_printf("Time stands still while off.");

        bool acted = false;
        while (!acted) {
            uint16_t joy1, joy2, hid1, hid2;
            get_joypad_states(&joy1, &joy2, &hid1, &hid2);
            uint16_t p = joy1 | hid1 | joy2 | hid2;
            if (controls_poll(joy1 | hid1, joy2 | hid2, false, game_loaded()))
                return false;           // the menu combo: abandon the edit
            if (combo_in_progress(joy1 | hid1, joy2 | hid2)) {
                delay(20);
                continue;
            }
            if (p & BTN_A) {
                clock_set(dt);
                suppress_held_buttons();
                return true;
            }
            if (p & BTN_B) {
                suppress_held_buttons();
                return false;
            }
            if (p & BTN_UP) {
                field = (field + 4) % 5;      // wrap: Year -> Minute
                acted = true;
            } else if (p & BTN_DOWN) {
                field = (field + 1) % 5;      // wrap: Minute -> Year
                acted = true;
            } else if (p & (BTN_LEFT | BTN_RIGHT)) {
                int d = (p & BTN_RIGHT) ? 1 : -1;
                switch (field) {
                case 0: dt.year += d; if (dt.year < 2000) dt.year = 2099; if (dt.year > 2099) dt.year = 2000; break;
                case 1: dt.mon += d; if (dt.mon < 1) dt.mon = 12; if (dt.mon > 12) dt.mon = 1; break;
                case 2: dt.day += d; break;     // clamped/wrapped below
                case 3: dt.hour = (dt.hour + d + 24) % 24; break;
                case 4: dt.min = (dt.min + d + 60) % 60; break;
                }
                if (field == 2) {
                    int dim = datetime_days_in_month(dt.year, dt.mon);
                    if (dt.day < 1) dt.day = dim;
                    if (dt.day > dim) dt.day = 1;
                }
                acted = true;
            }
            if (!acted)
                delay(20);
        }
        suppress_held_buttons();        // one press, one step
        delay(50);
    }
}

struct OptionsMenu: ScrollMenu {
    Settings edit;
    std::string message;

    OptionsMenu() : edit(settings) {}

    const char *menu_title() override {
        return "         --- Options ---";
    }

    int item_count() override {
        return 13;
    }

    std::string item_text(int i) override {
        char buf[40];
        switch (i) {
        case 0: snprintf(buf, sizeof buf, "Menu:  %s", combo_to_string(edit.menu_combo, true).c_str()); break;
        case 1: snprintf(buf, sizeof buf, "Reset: %s", combo_to_string(edit.reset_combo, true).c_str()); break;
        case 2: snprintf(buf, sizeof buf, "Reset combo: %s", edit.reset_enabled ? "ON" : "OFF"); break;
        case 3: snprintf(buf, sizeof buf, "Hold to close: %lu s", (unsigned long)(edit.close_hold_ms / 1000)); break;
        case 4: snprintf(buf, sizeof buf, "Diagnostics: %s", edit.diag ? "ON" : "OFF"); break;
        case 5: snprintf(buf, sizeof buf, "Display: %s", edit.display120 ? "120 Hz (480p)" : "60 Hz (720p)"); break;
        case 6: snprintf(buf, sizeof buf, "Pause in game menu: %s", edit.pause_in_menu ? "ON" : "OFF"); break;
        case 7: {
            DateTime dt;
            clock_now(dt);
            char when[20];
            clock_format_short(when, sizeof when, dt);
            snprintf(buf, sizeof buf, "Date & time: %s", when);
            break;
        }
        case 8: snprintf(buf, sizeof buf, "Flash mode..."); break;
        case 9: snprintf(buf, sizeof buf, "USB debug console: %s", usb_console_enabled() ? "ON" : "OFF"); break;
        case 10: snprintf(buf, sizeof buf, "Save debug log"); break;
        case 11: snprintf(buf, sizeof buf, "Save"); break;
        default: snprintf(buf, sizeof buf, "<< Back"); break;
        }
        return buf;
    }

    void render_footer() override {
        if (!message.empty()) {
            overlay_cursor(2, MENU_FOOTER_ROW);
            overlay_printf("%s", message.c_str());
        }
        //              01234567890123456789012345678901
        overlay_cursor(2, 23);
        overlay_printf("Menu combo opens the game menu");
        overlay_cursor(2, 24);
        overlay_printf("Reset resets, hold closes game");
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

    bool item_chosen(int i) override {
        message = "";
        switch (i) {
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
            edit.display120 = !edit.display120;
            message = "Takes effect on next load";
            break;
        case 6:
            edit.pause_in_menu = !edit.pause_in_menu;
            break;
        case 7:
            if (edit_datetime()) {
                extern int16_t active_core;
                message = settings_save() ? "Clock set" : "Save failed. Read-only drive?";
                clock_send_to_core(active_core == 3 ? gba_current_gpio_flags() : 0);
            }
            break;
        case 8:
            push_menu(std::unique_ptr<Menu>(new FlashModeMenu()));
            return false;
        case 9:                     // USB debug console on/off
            if (usb_console_enabled()) {
                usb_console_set_enabled(false);
                message = "USB console OFF; host back";
            } else if (usb_console_set_enabled(true)) {
                message = "USB console ON: BL616 pads off";
            } else {
                message = "Needs the SD card (USB drive)";
            }
            break;
        case 10: {                  // Save debug log
            if (saves_busy()) {
                message = "Save in progress, retry";
                break;
            }
            std::string dir = std::string(drv) + "tangcore";
            f_mkdir(dir.c_str());   // FR_EXIST is fine
            dir += "/";
            message = dbg_log_save(dir.c_str())
                          ? "Wrote tangcore/debuglog.txt"
                          : "Could not write the log";
            dbg_log("options: save debug log -> %s", message.c_str());
            break;
        }
        case 11:
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
    dbg_log("scanline preview: enter");
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
    dbg_log("scanline preview: exit, changed=%d pending=%d",
            changed ? 1 : 0, (int)pending_action);
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
        overlay_printf("Preview");
        overlay_cursor(2, 14);
        overlay_printf("<< Back");
        overlay_cursor(2, 16);
        //                01234567890123456789012345678901
        overlay_printf("Preview hides this menu:");
        overlay_cursor(2, 17);
        overlay_printf(" LEFT/RIGHT  darkness");
        overlay_cursor(2, 18);
        overlay_printf(" UP/DOWN     thin/thick");
        overlay_cursor(2, 19);
        overlay_printf(" SELECT      on/off");
        overlay_cursor(2, 20);
        overlay_printf(" A or B      back here");
        if (!message.empty()) {
            overlay_cursor(2, 22);
            overlay_printf("%s", message.c_str());
        }
    }

    std::vector<int> get_options() override {
        return {9, 10, 11, 12, 14};
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

// Show the game without the menu while the pads adjust a filter: step()
// applies one press and returns true if a setting changed. Returns true if
// anything changed. Paused, except on SNES/MD (live, pads muted): same as
// the scanline preview.
static bool video_preview(bool (*step)(uint16_t p)) {
    dbg_log("video preview: enter");
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
    dbg_log("video preview: exit, changed=%d pending=%d",
            changed ? 1 : 0, (int)pending_action);
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
            overlay_cursor(2, 24);
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
    bool changed = false;
    std::string message;
    bool bfi;       // a 120 Hz build is running: offer the BFI row

    VideoMenu() {
        // The firmware loads specific files from Cores verbatim, so the
        // running bitstream decides: only a _120hz build gets BFI.
        bfi = loaded_core_is_120hz();
        rows = lcd_grid_offered() ? std::vector<int>{9, 10, 11, 12, 13}
                                  : std::vector<int>{9, 10, 11, 13};
        if (bfi)
            rows.push_back(14);
        rows.push_back(bfi ? 16 : 15);
    }

    ~VideoMenu() {
        if (changed)
            settings_save();
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
        overlay_cursor(2, 13);
        overlay_printf("Scale: %s", settings.scale_full ? "Full" : "Integer");
        if (bfi) {
            overlay_cursor(2, 14);
            overlay_printf("BFI: %s", bfi_mode_name(settings.bfi_mode));
            overlay_cursor(2, 16);
        } else {
            overlay_cursor(2, 15);
        }
        overlay_printf("<< Back");
        if (!message.empty()) {
            overlay_cursor(2, 17);
            overlay_printf("%s", message.c_str());
        }
    }

    std::vector<int> get_options() override {
        return rows;
    }

    bool on_choose(int idx) override {
        int row = rows[idx];
        message = "";
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
        } else if (row == 13) {         // Full, Integer
            settings.scale_full = !settings.scale_full;
            changed = true;
            apply_core_config();
            do_redraw();
            return false;
        } else if (row == 14) {         // Off, Black, Dim, Rolling
            settings.bfi_mode = (settings.bfi_mode + 1) & 3;
            changed = true;
            apply_core_config();
            do_redraw();
            return false;
        }
        // << Back: the game menu stays loaded
        if (changed && !settings_save()) {
            changed = false;            // Back again leaves without saving
            message = "Save failed. Read-only drive?";
            do_redraw();
            return false;
        }
        changed = false;
        return true;
    }
};

Menu *create_video_menu(void) {
    return new VideoMenu();
}

void menu_options(void) {
    dbg_log("options: open");
    menu_clear();
    push_menu(std::unique_ptr<Menu>(new OptionsMenu()));
    menu_current()->do_redraw();
    suppress_held_buttons();    // the button that opened Options
    menu_input_loop();
    dbg_log("options: exit, pending=%d", (int)pending_action);
    menu_clear();
}
