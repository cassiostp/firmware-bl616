/*
 * TangCore firmware for BL616 MCU
 *
 * (c) 2025, nand2mario <nand2mario@outlook.com>
 *
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree. 
 *
 */

#include <stdarg.h>
#include <string.h>
#include <vector>

extern "C" {
#include "board.h"
#include "bl616_glb.h"
#include "bl616_hbn.h"
#include "bflb_gpio.h"
#include "bflb_uart.h"
#include "bflb_clock.h"
#include "bl616_clock.h"

#include "usbh_core.h"
#include "ff.h"
#include "fatfs_diskio_register.h"
}

#include "file_chooser.h"
#include "programmer.h"
#include "usb_gamepad.h"
#include "utils.h"
#include "cores.h"
#include "overlay.h"
#include "init.h"
#include "menu_manager.h"
#include "settings.h"
#include "game_controls.h"

// Uncomment this to enable UART console (use with caution. it may interfere with MCU-FPGA communication)
#define UART_CONSOLE

/////////////////////////////////////////////////////////////////////////////////
// Global state

int16_t active_core = -1;           // firmware detected this core as active
bool core_running;                  // a rom is loaded and running on the core
struct core_info *core;

// UART
struct bflb_device_s *gpio_dev;
struct bflb_device_s *uart0_dev;
struct bflb_device_s *uart1_dev;
struct bflb_device_s *wdg_dev;

// USB and fatfs
struct usbh_msc *msc;
const char *drv = "sd:";

// Tasks and shared state
TaskHandle_t main_task_handle;
TaskHandle_t uart1_rx_task_handle;

#ifdef TANG_CONSOLE60K
const char *BOARD_NAME = "console60k";
#elif defined(TANG_CONSOLE138K)
const char *BOARD_NAME = "console138k";
#elif defined(TANG_MEGA60K)
const char *BOARD_NAME = "mega60k";
#elif defined(TANG_MEGA138K)
const char *BOARD_NAME = "mega138k";
#elif defined(TANG_PRIMER25K)
const char *BOARD_NAME = "primer25k";
#elif defined(TANG_NANO20K)
const char *BOARD_NAME = "nano20k";
#else
const char *BOARD_NAME = "unknown";
#endif

// Override system printf() to send to FPGA
int __attribute__((weak)) putchar(int ch) {
    fpga_tx_header(0x05, 2);
    fpga_tx_byte(ch);
    return ch;
}


/////////////////////////////////////////////////////////////////////////////////
// Core loading and other file system operations

// nand2mario: these USB data structures cannot be CACHED as they are written to by hardware
USB_NOCACHE_RAM_SECTION FATFS fs;
USB_NOCACHE_RAM_SECTION FIL fcore;
USB_NOCACHE_RAM_SECTION BYTE __attribute__((aligned(64))) fbuf[BLOCK_SIZE];

FRESULT res_sd;
FileChooser file_chooser;

// The game that is running, so MODE and the reset combo can reload it
static struct core_info *last_core;     // NULL if no ROM was loaded
static string last_rom;
static string last_core_file;           // bitstream of the running core

bool game_loaded(void) {
    return last_core != NULL || !last_core_file.empty();
}

// #define PAGESIZE 22
// #define TOPLINE 2
// #define PWD_SIZE 1024
// char pwd[PWD_SIZE];
// one page of file names to display
// char file_names[PAGESIZE][256];
// int file_dir[PAGESIZE];         // this file is a directory
// int file_sizes[PAGESIZE];       
// int file_len;		            // number of files on this page

/////////////////////////////////////////////////////////////////////////////////
// Menu display and user interaction

static void clear_pad_states(void);

// Program a bitstream. The new core is silent while it starts, and pads read
// by the FPGA restart from nothing, so reset what tracks them.
static bool program_fpga(const char *fname) {
    bool r = fpga_program(fname);
    clear_pad_states();
    controls_reset();
    return r;
}

// The FPGA no longer runs the recorded game (a new bitstream was programmed)
static void forget_game(void) {
    last_core = NULL;
    last_rom.clear();
    last_core_file.clear();
    core_running = false;
    gba_bios_loaded = false;            // the new bitstream doesn't have it
    forget_floppies();
}

// Pads read by the FPGA only report changes, so a button held while the FPGA
// is reprogrammed would stay "held". Clear them.
static void clear_pad_states(void) {
    if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
        joy1_state = 0;
        joy2_state = 0;
        xSemaphoreGive(state_mutex);
    }
}

static void send_hid_state(uint16_t hid1, uint16_t hid2) {
    taskENTER_CRITICAL();
    fpga_tx_header(0x09, 5);
    fpga_tx_byte(hid1 >> 8);
    fpga_tx_byte(hid1 & 0xff);
    fpga_tx_byte(hid2 >> 8);
    fpga_tx_byte(hid2 & 0xff);
    taskEXIT_CRITICAL();
}

// Load `rom` on `core`, programming the core's bitstream first if it isn't
// running (or always, with force_program). Returns 1 if the ROM was loaded,
// -1 on error.
static int load_game(core_info *core, const string &rom, bool force_program) {
    // Check the extension before touching the FPGA. Programming the core
    // first and then rejecting the file leaves the new core running with
    // nothing loaded and an undismissable error box.
    if (!core_supports_file(core, rom.c_str())) {
        overlay_message("Unsupported file type", 1);
        return -1;
    }

    active_core = get_core_id();

    if (force_program || active_core != core->id) {
        string fname_core;
        if (find_core_for_board(fname_core, core->core_file)) {
            program_fpga(fname_core.c_str());
            forget_game();                  // new bitstream, nothing loaded yet
            last_core_file = fname_core;
            _overlay_on = 1;

            // allow 2 seconds for core to start
            uint64_t start = bflb_mtimer_get_time_ms();
            while (bflb_mtimer_get_time_ms() - start < 2000) {
                send_blank_packet();
                heartbeat_bump();
                active_core = get_core_id();
                if (active_core == core->id)
                    break;
            }
        }
    }

    if (active_core == core->id) {
        overlay_status("Loading ROM: %s\n", rom.c_str());
        string loading = rom;               // rom may be last_rom itself
        if (core->load_rom(loading.c_str()) != 0)
            return -1;                      // the loader showed the error
        // Drive the video options now that the core runs. A fresh core
        // defaults every core_config bit to 0, and the low 16 bits stay as
        // they are (core specific).
        apply_core_config();
        last_core = core;
        last_rom = loading;
        return 1;
    } else {
        overlay_status("Core failed to load\n");
        delay(1000);
        return -1;
    }
}

// Menus for "NES", "SNES" ... entries
// dir: initial dir including the drive name (e.g. "sd:nes", "usb:cores")
// return 0: user chose a ROM (*choice), 1: no choice made, -1: error
// file chosen: pwd / file_name[*choice]
static int menu_loadrom(const char *dir) {
    string fname;
    file_chooser.rootdir = dir;
    file_chooser.curdir = dir;
    file_chooser.msg_return = "<< Return to main menu";
    // Hide files no loader accepts. Match the entry dir against the cores'
    // ROM dirs ("snes" contains "nes", so match the full last component).
    // The Cores browser matches none of them and lists everything, as before.
    file_chooser.filter_exts.clear();
    {
        string d = dir;
        for (auto &c : core_info_list) {
            string a = string(":") + c.rom_dir, b = string("/") + c.rom_dir;
            if ((d.size() >= a.size() && d.compare(d.size() - a.size(), a.size(), a) == 0) ||
                (d.size() >= b.size() && d.compare(d.size() - b.size(), b.size(), b) == 0)) {
                file_chooser.filter_exts = c.rom_exts;
                break;
            }
        }
    }
    bool r = file_chooser.choose_file(fname);
    if (!r) {
        overlay_status("No file chosen");
        return 1;
    }

    // now proceed to load the core and ROM
    joy1_state = 0; joy2_state = 0; // clear joypad states

    // load core if in cores/ dir
    if (fname.find(string(drv) + "cores") == 0) {
        overlay_status("Core: %s", fname.c_str());
        program_fpga(fname.c_str());
        forget_game();
        last_core_file = fname;
        _overlay_on = 1;                // turn on overlay after core is loaded
        // drive the video options once the new core answers with its ID
        uint64_t start = bflb_mtimer_get_time_ms();
        while (bflb_mtimer_get_time_ms() - start < 2000) {
            send_blank_packet();
            active_core = get_core_id();
            if (active_core >= 0)
                break;
        }
        apply_core_config();
        return 0;       // return to main menu
    } 

    // find core info entry
    core_info *core = NULL;
    string path = fname.substr(fname.find(":")+1);
    for (int i = 0; i < core_info_list.size(); i++) {
        core_info *c = &core_info_list[i];
        if (path.find(c->rom_dir) == 0) {
            overlay_status("ROM for: %s", c->display_name);
            core = c;
            break;
        }
    }
    if (core == NULL) {
        overlay_status("Core not found: %s", path.c_str());
        return -1;
    }

    return load_game(core, fname, false);
}

// keep sending HID state to core until OSD is turned on
static void send_hid_to_core(void) {
    uint16_t hid1_old = 0, hid2_old = 0;
    bool first = true;
    dprint("Start sending HID to core...");
    while (1) {
        uint16_t joy1=0, joy2=0, hid1=0, hid2=0;    
        get_joypad_states(&joy1, &joy2, &hid1, &hid2);
        if (first || hid1 != hid1_old || hid2 != hid2_old) {    // send HID if changed
            send_hid_state(hid1, hid2);
            hid1_old = hid1;
            hid2_old = hid2;
            first = false;
        }
        if (overlay_on()) {     // turned on by the keyboard's OSD key (F12)
            if (game_loaded())
                pending_action = ACTION_GAME_MENU;
            break;
        }
        if (controls_poll(joy1 | hid1, joy2 | hid2, true, game_loaded())) {
            if (pending_action != ACTION_RESET)
                send_hid_state(0, 0);   // the core mustn't keep the combo held
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    dprint("Stopped sending HID to core.");
}

// Diagnostic line on row 0 (row 26 sits under some cores' logos, hiding it).
// Shows raw pad states (FPGA pads J, USB pads H), the pending action and
// whether a combo is held (a), the core ID (c), and a counter that stops if
// this loop stops running.
static void draw_diag_line(uint16_t joy1, uint16_t joy2, uint16_t hid1, uint16_t hid2) {
    static uint64_t last_draw;
    static uint32_t beat;
    uint64_t now = bflb_mtimer_get_time_ms();
    if (!settings.diag || now - last_draw < 250)
        return;
    last_draw = now;
    overlay_cursor(0, 0);
    // fixed width, exactly 32 columns: J00000000 H00000000 a00 c0  #000
    overlay_printf("J%04x%04x H%04x%04x a%d%d c%-2d #%03lu", joy1, joy2, hid1, hid2,
                   (int)pending_action, combo_in_progress(joy1 | hid1, joy2 | hid2) ? 1 : 0,
                   active_core, (unsigned long)(beat++ % 1000));
}

// // (R L X A RT LT DN UP START SELECT Y B)
// Return: 1 button B pressed, 4: button A pressed, 2: next page, 3: previous page
// active is the entry chosen
int joy_choice(int start_line, int len, int *active) {
    if (*active < 0 || *active >= len)
        *active = 0;
    uint16_t joy1=0, joy2=0, hid1=0, hid2=0;    
    int last = *active;

    get_joypad_states(&joy1, &joy2, &hid1, &hid2);
    if (overlay_on())
        draw_diag_line(joy1, joy2, hid1, hid2);
    joy1 |= hid1;
    joy2 |= hid2;

    // Draw the cursor before any early return: a pending action or a combo
    // in progress used to leave the menu on screen with no cursor and dead
    // navigation.
    if (overlay_on()) {
        overlay_cursor(0, start_line + (*active));
        overlay_printf(">");
    }

    if (overlay_on() && controls_poll(joy1, joy2, false, game_loaded()))
        return 0;                  // menu combo: the caller handles pending_action
    if (overlay_on() && combo_in_progress(joy1, joy2))
        return 0;                  // its buttons aren't navigation

    if (!overlay_on()) {           // keep sending HID state to core when OSD is off
        send_hid_to_core();
        return 0;
    }

    if ((joy1 & 0x10) || (joy2 & 0x10)) {
        if (*active > 0) (*active)--;
    }
    if ((joy1 & 0x20) || (joy2 & 0x20)) {
        if (*active < len-1) (*active)++;
    }
    if ((joy1 & 0x40) || (joy2 & 0x40))
        return 3;      // previous page
    if ((joy1 & 0x80) || (joy2 & 0x80))
        return 2;      // next page
    if ((joy1 & 0x100) || (joy2 & 0x100))
        return 4;      // button A pressed
    if ((joy1 & 0x1) || (joy2 & 0x1))
        return 1;      // button B pressed

    // overlay_cursor(0, 27);
    // overlay_printf(" j1=%04x j2=%04x h1=%04x h2=%04x", joy1, joy2, hid1, hid2);
    if (last != *active) {
        overlay_cursor(0, start_line + last);
        overlay_printf(" ");
        overlay_cursor(0, start_line + (*active));
        overlay_printf(">");
        delay(100);     // button debounce
    }    
    return 0;
}

#define MAIN_TASK_STACK_SIZE  2048
#define MAIN_TASK_PRIORITY    3
#define UART1_RX_TASK_STACK_SIZE  512
#define UART1_RX_TASK_PRIORITY    3
#define WATCHDOG_TASK_STACK_SIZE  256
#define WATCHDOG_TASK_PRIORITY    4       // above main_task: feeds it mustn't miss
#define HEARTBEAT_STALL_MS        15000   // stop feeding after this long without a beat
#define WATCHDOG_FEED_MS          2000    // hardware timeout after feeding stops

// Feed the hardware watchdog while the heartbeat keeps moving (something is
// reading pads, listing files or sending data). If it stops - a crash, a spin
// loop, a mutex deadlock - stop feeding and the watchdog resets the chip.
static void watchdog_task(void *pvParameters)
{
    uint32_t last_beat = heartbeat;
    uint64_t last_beat_time = bflb_mtimer_get_time_ms();
    while (1) {
        if (heartbeat != last_beat) {
            last_beat = heartbeat;
            last_beat_time = bflb_mtimer_get_time_ms();
        }
        if (bflb_mtimer_get_time_ms() - last_beat_time < HEARTBEAT_STALL_MS)
            bflb_wdg_reset_countervalue(wdg_dev);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Receive joypad updates and other UART responses from the FPGA
static void uart1_rx_task(void *pvParameters)
{
    uint8_t buffer[5];
    uint16_t pos = 0;
    uint8_t type = 0;
    uint16_t len = 0;
    uint64_t last_byte = 0;
    
    while (1) {
        if (bflb_uart_rxavailable(uart1_dev)) {
            uint8_t ch = bflb_uart_getchar(uart1_dev);
            uint64_t now = bflb_mtimer_get_time_ms();
            // a frame's bytes arrive back to back at 2 Mbaud. A long gap means
            // the rest was lost (e.g. the FPGA was reconfigured): resync. The
            // margin covers this task being starved while ROM data is sent.
            if (pos != 0 && now - last_byte > 50)
                pos = 0;
            last_byte = now;
            
            if (pos == 0) {          // expecting 0xAA
                if (ch == 0xAA) 
                    pos++;
            } else if (pos == 1) {   // len msb
                len = (uint16_t)ch << 8;
                pos++;
            } else if (pos == 2) {   // len lsb
                len += ch;
                pos++;
            } else if (pos == 3) {   // command type
                type = ch;
                pos++;

            ////// pos >= 4 //////
            } else if (type == 1) {     // response to command 1 (get core ID)
                if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
                    core_id = ch;
                    xSemaphoreGive(state_mutex);
                }
                pos = 0;
            } else if (type == 2) {                 // config string
                // skip for now
                if (pos == len+2)
                    pos = 0;
                else
                    pos++;
            } else if (type == 3) {                 // periodic joypad state
                buffer[pos-4] = ch;
                // Complete packet received
                if (pos == 7) {
                    // Combine bytes into 16-bit values
                    uint16_t joy1 = (buffer[0] << 8) | buffer[1];
                    uint16_t joy2 = (buffer[2] << 8) | buffer[3];
                    
                    // Update global state with mutex protection
                    if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
                        joy1_state = joy1;
                        joy2_state = joy2;
                        xSemaphoreGive(state_mutex);
                    }
                    pos = 0; // Reset for next packet
                } else
                    pos++;
            } else if (type == 4) {              // floppy write
                if (pos < 6)
                    buffer[pos-4] = ch;
                else
                    fbuf[pos-6] = ch;
                if (pos == 6+511) {
                    uint16_t drive = buffer[0] >> 7;
                    uint16_t sector = (buffer[0] & 0x7f) << 8 | buffer[1];
                    if (floppy[drive]) {
                        UINT br;
                        f_lseek(&f_floppy[drive], sector * 512);
                        if (f_write(&f_floppy[drive], fbuf, 512, &br) != FR_OK) {
                            overlay_status("Failed to write floppy");
                        }
                    }
                    pos = 0;   // reset for next packet
                } else
                    pos++;
            } else if (type == 5) {              // floppy read
                buffer[pos-4] = ch;
                if (pos == 5) {
                    uint16_t drive = buffer[0] >> 7;
                    uint16_t sector = (buffer[0] & 0x7f) << 8 | buffer[1];
                    if (floppy[drive]) {
                        UINT br;
                        f_lseek(&f_floppy[drive], sector * 512);
                        if (f_read(&f_floppy[drive], fbuf, 512, &br) == FR_OK) {
                            taskENTER_CRITICAL();   // don't interleave with main_task's frames
                            fpga_tx_header(0x0a, br+1);
                            for (UINT i = 0; i < br; i++) {
                                fpga_tx_byte(fbuf[i]);
                            }
                            taskEXIT_CRITICAL();
                        } else {
                            overlay_status("Failed to read floppy");
                        }
                    }
                    pos = 0;   // reset for next packet
                } else
                    pos++;

            } else {
                pos = 0; // Reset if we get out of sync
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Close the game: load the menu core again, as at boot.
static void close_game(void) {
    overlay(1);
    overlay_status("Closing game...");
    string fname;
    if (find_core_for_board(fname, "monitor.bin"))
        program_fpga(fname.c_str());
    forget_game();
    active_core = get_core_id();
}

static core_info *loaded_core(void) {
    return last_core ? last_core : find_core_by_id(active_core);
}

// Reset the running game. Holding the core in loading state with no data, then
// releasing it, restarts it with the ROM still in memory: the same path as
// loading a second ROM, minus the data. Genesis can't: an empty load sets its
// ROM size to 0 (mdtang_top.sv), so it reloads the ROM.
static void reset_game(void) {
    if (!last_core) {                       // a core from Cores: restart it
        if (!last_core_file.empty()) {
            string fname = last_core_file;
            program_fpga(fname.c_str());
            forget_game();
            last_core_file = fname;
            active_core = get_core_id();
            apply_core_config();       // the new bitstream starts with 0
        }
        return;
    }
    overlay_status("Resetting %s", last_core->display_name);
    if (last_core->id == 4) {
        load_game(last_core, last_rom, false);  // turns the overlay off when done
        return;
    }
    set_loading_state(1);
    delay(20);
    set_loading_state(0);
    overlay(0);
}

// Menu over the running game: the menu combo opens it in game, and the main
// menu links to it while a game is loaded.
struct GameMenu: Menu {
    std::string message;
    bool pcxt;
    std::vector<int> rows;

    GameMenu() {
        core_info *core = loaded_core();
        pcxt = core && core->id == 6;
        rows = pcxt ? std::vector<int>{9, 10, 11, 12, 13, 15, 17}
                    : std::vector<int>{9, 10, 11, 12, 15, 17};
    }

    void render() override {
        core_info *core = loaded_core();
        overlay_clear();
        overlay_cursor(0, 7);
        //              01234567890123456789012345678901
        overlay_printf("  --- %s ---", core ? core->display_name : "Game");
        overlay_cursor(2, 9);
        overlay_printf("Resume");
        overlay_cursor(2, 10);
        overlay_printf("Reset");
        overlay_cursor(2, 11);
        overlay_printf("Game options (soon)");
        overlay_cursor(2, 12);
        overlay_printf("Save states (soon)");
        if (pcxt) {
            overlay_cursor(2, 13);
            overlay_printf("Floppy drives...");
        }
        overlay_cursor(2, 15);
        overlay_printf("Close game");
        overlay_cursor(2, 17);
        overlay_printf("<< Main menu");
        if (!message.empty()) {
            overlay_cursor(2, 20);
            overlay_printf("%s", message.c_str());
        }
    }

    std::vector<int> get_options() override {
        return rows;
    }

    bool on_choose(int idx) override {
        int row = rows[idx];
        if (row == 9) {
            pending_action = ACTION_RESUME;
            return true;
        } else if (row == 10) {
            pending_action = ACTION_RESET;
            return true;
        } else if (row == 11 || row == 12) {
            message = "Not available yet";
            do_redraw();
            return false;
        } else if (row == 13) {
            core_info *core = loaded_core();
            string dir = string(drv).append(core->rom_dir);
            push_menu(std::unique_ptr<Menu>(create_pcxt_menu(dir.c_str())));
            return false;
        } else if (row == 15) {
            pending_action = ACTION_CLOSE;
            return true;
        }
        return true;                    // << Main menu: the game stays loaded
    }
};

static void show_game_menu(void) {
    overlay(1);
    suppress_held_buttons();            // the button that opened it isn't a choice
    menu_clear();
    push_menu(std::unique_ptr<Menu>(new GameMenu()));
    menu_current()->do_redraw();
    menu_input_loop();
    menu_clear();
}

// Act on what the controller or a menu asked for. Returns true if anything
// was done, and the main menu should be redrawn.
static bool handle_game_action(void) {
    bool acted = false;
    for (;;) {
        GameAction action = pending_action;
        pending_action = ACTION_NONE;
        switch (action) {
        case ACTION_GAME_MENU:
            show_game_menu();           // may set another action
            break;
        case ACTION_RESUME:
            menu_clear();
            if (game_loaded())
                overlay(0);             // the main loop goes back to the game
            break;
        case ACTION_RESET:
            menu_clear();
            reset_game();
            break;
        case ACTION_CLOSE:
            menu_clear();
            close_game();
            break;
        default:
            return acted;
        }
        acted = true;
    }
}

// Display main menu and call other menu functions
static void main_task(void *pvParameters)
{
    uint32_t last_redraw_time = 0;
    // volatile uint32_t *reg_gpio0 = (volatile uint32_t *)0x200008c4;     // bl616 reference 4.8.5
    // volatile uint32_t *reg_gpio1 = (volatile uint32_t *)0x200008c8;
    // volatile uint32_t *reg_gpio2 = (volatile uint32_t *)0x200008cc;
    // volatile uint32_t *reg_gpio3 = (volatile uint32_t *)0x200008d0;

    // wait for drive to be ready
    uint64_t start = bflb_mtimer_get_time_ms();
    FRESULT res;
    overlay_status("Mounting sd card...", drv);
    while ((res = f_mount(&fs, "sd:", 1)) != FR_OK && bflb_mtimer_get_time_ms() - start < 500) {
        heartbeat_bump();
        delay(100);
    }

    if (res == FR_OK) {
        overlay_status("SD card mounted in %d ms", bflb_mtimer_get_time_ms() - start);
    } else  {
        overlay_status("SD not found. Mounting USB...");
        drv = "usb:";
        start = bflb_mtimer_get_time_ms();
        while ((res = f_mount(&fs, "usb:", 1)) != FR_OK && bflb_mtimer_get_time_ms() - start < 2000) {
            heartbeat_bump();
            delay(100);
        }
        if (res != FR_OK) {
            overlay_status("Failed to mount USB drive");
        } else {
            overlay_status("USB drive mounted in %d ms", bflb_mtimer_get_time_ms() - start);
        }
    }

    settings_load();

    // load monitor core at startup
    string fname;
    if (find_core_for_board(fname, "monitor.bin")) {
        program_fpga(fname.c_str());
    } else {
        overlay_status("No monitor.bin found for board.");
    }

    controls_reset();

    while (1) {
        bool redraw = true;
        int choice = 0;
        // main menu items: core IDs, -1 Cores, -2 Options, -3 the game menu
        std::vector<int16_t> items;
        int line_start = 0;
        for (;;) {
            uint32_t now = bflb_mtimer_get_time_ms();
            if (active_core == -1) {
                active_core = get_core_id();            // 200ms timeout
                overlay_status("core_id=%d", active_core);
                if (active_core >= 0) redraw = true;    // redraw immediately if core is detected
            }
            // Poll the core ID without blanking the menu: the old code set
            // redraw = true every 5 s, and the redraw's overlay_clear()
            // blinked the menu off briefly.
            if (now - last_redraw_time > 5000) {
                last_redraw_time = now;
                int16_t id = get_core_id();
                if (id >= 0 && id != active_core) {
                    active_core = id;
                    redraw = true;
                }
            }
            if (redraw) {
                active_core = get_core_id();            // allow jtag to change core underneath us
                overlay(overlay_on());                  // set correct overlay state
                overlay_clear();

                items.clear();
                core_info *game = loaded_core();
                if (game_loaded())
                    items.push_back(-3);
                items.insert(items.end(), main_menu_config.begin(), main_menu_config.end());
                line_start = 13 - (items.size()+2+2) / 2;   // 2 lines for version, 2 for "TangCore"

                int line = line_start;
                overlay_cursor(0, line++);
                //              01234567890123456789012345678901
                overlay_printf("       -== TangCore ==-");
                line++;

                // display all menu items
                for (size_t i = 0; i < items.size(); i++) {
                    overlay_cursor(2, line++);
                    if (items[i] > 0) {
                        core_info *c = find_core_by_id(items[i]);
                        if (c)
                            overlay_printf("%s", c->display_name);
                    } else if (items[i] == -1) {
                        overlay_printf("Cores");
                    } else if (items[i] == -2) {
                        overlay_printf("Options");
                    } else if (items[i] == -3) {
                        if (game && strlen(game->display_name) <= 18)     // 32 columns
                            overlay_printf("Game menu (%s)", game->display_name);
                        else
                            overlay_printf("Game menu");
                    }
                }

                line++;
                overlay_cursor(2, line++);
                overlay_printf("Version: ");
                overlay_printf(__DATE__);
                last_redraw_time = now;
                redraw = false;
            }

            int r = joy_choice(line_start+2, items.size(), &choice);
            if (handle_game_action()) {
                redraw = true;
                continue;
            }
            if (r == 1) break;

            delay(20);
        }

        int16_t item = items[choice];
        if (item > 0) {
            // Load rom or core from USB drive
            struct core_info *core = find_core_by_id(item);
            if (core) {
                std::string dir = std::string(drv).append(core->rom_dir);
                menu_loadrom(dir.c_str());
            }
        } else if (item == -1) {
            // load cores manually
            std::string dir = std::string(drv).append("cores");
            menu_loadrom(dir.c_str());
        } else if (item == -2) {
            // Options
            menu_options();
        } else if (item == -3) {
            pending_action = ACTION_GAME_MENU;
        }
        handle_game_action();               // game menu, or a combo in the file browser

        delay(300);
    }
}

static void print_system_info(void) {
    // this is viewable with scripts/liveuart.py
    overlay_status("TangCore %s", __DATE__);
    overlay_status("TangBoard: %s", BOARD_NAME);
    overlay_status("System clock: %u MHz", bflb_clk_get_system_clock(BL_SYSTEM_CLOCK_MCU_CLK) / 1000000);
    // UART registers
    // Clock comes from XCLK/160M/BCLK and goes through a divider and becomes UART_CLK
    overlay_status("GLB_UART_CFG0: %08x", BL_RD_WORD(0x20000150));
    overlay_status("GLB_UART_CFG1: %08x", BL_RD_WORD(0x20000154));
    overlay_status("GLB_UART_CFG2: %08x", BL_RD_WORD(0x20000158));
    overlay_status("UART0 clock: %u", Clock_Peripheral_Clock_Get(BL_PERIPHERAL_CLOCK_UART0));
    overlay_status("UART1 clock: %u", Clock_Peripheral_Clock_Get(BL_PERIPHERAL_CLOCK_UART1));

    // 10.3.5: baudrate = UART_clk / (uart_prd + 1)
    // This causes memory exception.
    // overlay_status("UART_BIT_PRD: %08x", BL_RD_WORD(0x40010008));
    //              01234567890123456789012345678901
    // overlay_status("                                ");
}

// Initialize things, then start main_task and uart1_rx_task to do actual work
int main(void)
{
    /* Board init */
    board_init();
    // a "Flash mode" request has done its job once this firmware runs again
    if (HBN_Get_User_Boot_Config() == 1)
        HBN_Set_User_Boot_Config(0);
    init_core_list();

    // Initialize GPIO and UART
    init_gpio_and_uart();

    print_system_info();

    // Create mutex for joypad states
    state_mutex = xSemaphoreCreateMutex();

    overlay_status("Initializing SDH...");
    fatfs_sdh_driver_register();        // calls SDH_Init()
    // f_mount(&fs_sd, "sd:", 0);          // registers SDMMC drive 

    // Initializing USB host...
    overlay_status("Initializing USB host...");
    usbh_initialize();
    fatfs_usbh_driver_register();
    usb_gamepad_init();

    // Hardware watchdog: ~2 s without a feed resets the chip. Feeding it is
    // the watchdog task's job, and that stops when the heartbeat does.
    wdg_dev = bflb_device_get_by_name("watchdog");
    struct bflb_wdg_config_s wdg_cfg = {
        .clock_source = WDG_CLKSRC_32K,
        .clock_div = 31,            // 32 kHz / 32 = 1 kHz: one count per ms
        .comp_val = WATCHDOG_FEED_MS,
        .mode = WDG_MODE_RESET,
    };
    bflb_wdg_init(wdg_dev, &wdg_cfg);
    bflb_wdg_start(wdg_dev);

    overlay_status("Creating tasks...");
    // Create the tasks
    xTaskCreate(main_task, "main_task", MAIN_TASK_STACK_SIZE, NULL, MAIN_TASK_PRIORITY, &main_task_handle);
    xTaskCreate(uart1_rx_task, "uart1_rx_task", UART1_RX_TASK_STACK_SIZE, NULL, UART1_RX_TASK_PRIORITY, &uart1_rx_task_handle);
    xTaskCreate(watchdog_task, "watchdog", WATCHDOG_TASK_STACK_SIZE, NULL, WATCHDOG_TASK_PRIORITY, NULL);
    
    vTaskStartScheduler();

    while (1) {
    }
}
