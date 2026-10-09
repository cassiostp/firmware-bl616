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

int option_osd_key = OPTION_OSD_KEY_SELECT_RIGHT;
int16_t active_core = -1;           // firmware detected this core as active
bool core_running;                  // a rom is loaded and running on the core
struct core_info *core;

// UART
struct bflb_device_s *gpio_dev;
struct bflb_device_s *uart0_dev;
struct bflb_device_s *uart1_dev;

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

// The FPGA no longer runs the recorded game (a new bitstream was programmed)
static void forget_game(void) {
    last_core = NULL;
    last_rom.clear();
    last_core_file.clear();
    core_running = false;
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
    active_core = get_core_id();

    if (force_program || active_core != core->id) {
        string fname_core;
        if (find_core_for_board(fname_core, core->core_file)) {
            fpga_program(fname_core.c_str());
            forget_game();                  // new bitstream, nothing loaded yet
            last_core_file = fname_core;
            _overlay_on = 1;

            // allow 2 seconds for core to start
            uint64_t start = bflb_mtimer_get_time_ms();
            while (bflb_mtimer_get_time_ms() - start < 2000) {
                send_blank_packet();
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
        fpga_program(fname.c_str());
        forget_game();
        last_core_file = fname;
        _overlay_on = 1;                // turn on overlay after core is loaded
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
    game_watch_start(last_core != NULL || !last_core_file.empty());
    while (1) {
        uint16_t joy1=0, joy2=0, hid1=0, hid2=0;    
        get_joypad_states(&joy1, &joy2, &hid1, &hid2);
        if (first || hid1 != hid1_old || hid2 != hid2_old) {    // send HID if changed
            send_hid_state(hid1, hid2);
            hid1_old = hid1;
            hid2_old = hid2;
            first = false;
        }
        if (joy1 == OSD_KEY_CODE || joy2 == OSD_KEY_CODE || hid1 == OSD_KEY_CODE || hid2 == OSD_KEY_CODE) {
            break;
        }
        if (overlay_on())      // turned off by keyboard
            break;
        if (game_watch_poll(joy1 | hid1, joy2 | hid2))     // combo or MODE
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    dprint("Stopped sending HID to core.");
}

// Diagnostic line on row 26 (status messages use row 27). Shows raw pad
// states (FPGA pads J, USB pads H), the core ID, how many times the overlay
// was hidden, and a counter that stops if this loop stops running.
static void draw_diag_line(uint16_t joy1, uint16_t joy2, uint16_t hid1, uint16_t hid2) {
    static uint64_t last_draw;
    static uint32_t beat;
    uint64_t now = bflb_mtimer_get_time_ms();
    if (!settings.diag || now - last_draw < 250)
        return;
    last_draw = now;
    overlay_cursor(0, 26);
    // fixed width, exactly 32 columns: J00000000 H00000000 c0  t00 #000
    overlay_printf("J%04x%04x H%04x%04x c%-2d t%02lu #%03lu", joy1, joy2, hid1, hid2, active_core,
                   (unsigned long)(overlay_hide_count % 100), (unsigned long)(beat++ % 1000));
}

// // (R L X A RT LT DN UP START SELECT Y B)
// Return: 1 button B pressed, 4: button A pressed, 2: next page, 3: previous page
// active is the entry chosen
int joy_choice(int start_line, int len, int *active, int overlay_key_code) {
    if (*active < 0 || *active >= len)
        *active = 0;
    uint16_t joy1=0, joy2=0, hid1=0, hid2=0;    
    int last = *active;

    get_joypad_states(&joy1, &joy2, &hid1, &hid2);
    if (overlay_on())
        draw_diag_line(joy1, joy2, hid1, hid2);
    joy1 |= hid1;
    joy2 |= hid2;

    if ((joy1 == overlay_key_code) || (joy2 == overlay_key_code)) {
        overlay_status("OSD: %s", overlay_on() ? "ON" : "OFF");
        overlay(!overlay_on());    // toggle OSD
        delay(300);
    }

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

    overlay_cursor(0, start_line + (*active));
    overlay_printf(">");

    // overlay_cursor(0, 27);
    // overlay_printf(" j1=%04x j2=%04x h1=%04x h2=%04x", joy1, joy2, hid1, hid2);
    if (last != *active) {
        overlay_cursor(0, start_line + last);
        overlay_printf(" ");
        delay(100);     // button debounce
    }    
    return 0;
}

#define MAIN_TASK_STACK_SIZE  2048
#define MAIN_TASK_PRIORITY    3
#define UART1_RX_TASK_STACK_SIZE  512
#define UART1_RX_TASK_PRIORITY    3

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

// Reprogram the running game's core and reload its ROM. Used after MODE
// reloaded the FPGA from flash, which drops the game.
static void reload_game(void) {
    overlay(1);
    clear_pad_states();
    if (last_core) {
        core_info *core = last_core;
        string rom = last_rom, drive_b;
        if (core->id == 6) {                // PC/XT: remount the floppies in use
            if (!floppy_path[0].empty())
                rom = floppy_path[0];
            drive_b = floppy_path[1];
        }
        if (load_game(core, rom, true) == 1 && !drive_b.empty())
            mount_floppy(1, drive_b.c_str());
    } else if (!last_core_file.empty()) {   // a core loaded from Cores, no ROM
        string fname = last_core_file;
        fpga_program(fname.c_str());
        forget_game();
        last_core_file = fname;
        _overlay_on = 1;
        active_core = get_core_id();
    }
}

// Reset the running game, keeping the ROM already in the core's memory.
// Toggling the loading state with no data holds the core in reset and then
// releases it; the ROM, mapper and size settings are kept (see each core's
// handling of rom_loading). Genesis can't do this: its loader zeroes the ROM
// size on a load with no data (mdtang_top.sv:171-178), so it reloads the ROM.
static void reset_game(void) {
    if (!last_core) {
        if (!last_core_file.empty())        // a core loaded from Cores, no ROM
            reload_game();
        return;
    }
    if (get_core_id() != last_core->id && get_core_id() != last_core->id) {
        reload_game();                      // the game's core is gone
        return;
    }

    switch (last_core->id) {
    case 4:                                 // Genesis: reload the ROM
        load_game(last_core, last_rom, false);
        break;
    case 3:                                 // GBA: stage 3 (config) resets without
        set_loading_state(3);               // touching the save type
        delay(20);
        set_loading_state(0);
        overlay(0);
        break;
    default:                                // NES, SNES, SMS, PC/XT
        set_loading_state(1);
        delay(20);
        set_loading_state(0);
        overlay(0);
        break;
    }
}

// Act on a combo or MODE press seen while a game was running.
// Returns true if the main menu should be redrawn.
static bool handle_game_action(void) {
    GameAction action = pending_action;
    pending_action = ACTION_NONE;
    switch (action) {
    case ACTION_MENU:                       // the game stays loaded behind the menu
        menu_clear();
        overlay(1);
        return true;
    case ACTION_MODE_MENU: {                // the game is gone: load the real menu core
        menu_clear();
        string fname;
        if (find_core_for_board(fname, "monitor.bin"))
            fpga_program(fname.c_str());
        forget_game();
        clear_pad_states();
        active_core = get_core_id();
        overlay(1);
        return true;
    }
    case ACTION_RESET:
        overlay_status("Resetting game");
        send_hid_state(0, 0);               // release the combo's buttons in the core
        clear_pad_states();
        reset_game();
        return true;
    case ACTION_MODE_RELOAD:
        overlay_status("FPGA reloaded, restarting game");
        reload_game();
        return true;
    default:
        return false;
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
    while ((res = f_mount(&fs, "sd:", 1)) != FR_OK && bflb_mtimer_get_time_ms() - start < 500)
        delay(100);

    if (res == FR_OK) {
        overlay_status("SD card mounted in %d ms", bflb_mtimer_get_time_ms() - start);
    } else  {
        overlay_status("SD not found. Mounting USB...");
        drv = "usb:";
        start = bflb_mtimer_get_time_ms();
        while ((res = f_mount(&fs, "usb:", 1)) != FR_OK && bflb_mtimer_get_time_ms() - start < 2000)
            delay(100);
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
        fpga_program(fname.c_str());
    } else {
        overlay_status("No monitor.bin found for board.");
    }

    int line_start;
    int menu_cnt = main_menu_config.size();
    line_start = 13 - (menu_cnt+2+2) / 2;       // 2 lines for version, 2 lines for "TangCore"

    while (1) {
        bool redraw = true;
        int choice = 0;
        for (;;) {
            uint32_t now = bflb_mtimer_get_time_ms();
            if (active_core == -1) {
                // send_blank_packet();
                active_core = get_core_id();            // 200ms timeout
                overlay_status("core_id=%d", active_core);
                if (active_core >= 0) redraw = true;    // redraw immediately if core is detected
            }
            // if (core < 0) continue;         // do not draw or process input if core is not ready
            if (now - last_redraw_time > 5000) 
                redraw = true;
            if (redraw) {
                active_core = get_core_id();            // allow jtag to change core underneath us
                overlay(overlay_on());                  // set correct overlay state
                overlay_clear();

                int line = line_start;
                overlay_cursor(0, line++);
                //              01234567890123456789012345678901
                overlay_printf("       -== TangCore ==-");
                line++;

                // display all menu items
                for (int i = 0; i < menu_cnt; i++) {
                    overlay_cursor(2, line++);
                    if (main_menu_config[i] > 0) {
                        for (int j = 0; core_info_list[j].id != 0; j++) {
                            if (core_info_list[j].id == main_menu_config[i]) {
                                overlay_printf("%s", core_info_list[j].display_name);
                                break;
                            }
                        }
                    } else if (main_menu_config[i] == -1) {
                        overlay_printf("Cores");
                    } else if (main_menu_config[i] == -2) {
                        overlay_printf("Options");
                    }
                }

                line++;
                overlay_cursor(2, line++);
                overlay_printf("Version: ");
                overlay_printf(__DATE__);
                last_redraw_time = now;
                redraw = false;

                // print some debug stats to UART
                // uint16_t joy1=0, joy2=0;
                // get_joypad_states(&joy1, &joy2);
                // overlay_status("core=%d, j1=%04x, j2=%04x", active_core, joy1, joy2);
                // overlay_status("Mtimer frequency: %d MHz", bflb_mtimer_get_freq() / 1000000);
                // overlay_status("CPU frequency: %d MHz", bflb_clk_get_system_clock(BL_SYSTEM_CLOCK_MCU_CLK) / 1000000);
                // overlay_status("GPIO0-3 status: %08x %08x %08x %08x", *reg_gpio0, *reg_gpio1, *reg_gpio2, *reg_gpio3);
            }

            bool before_overlay = overlay_on();
            int r = joy_choice(line_start+2, menu_cnt, &choice, OSD_KEY_CODE);
            if (handle_game_action()) {
                redraw = true;
                continue;
            }
            if (r == 1) break;

            if (!before_overlay && overlay_on() && active_core > 0) {
                // overlay is turned back on, now display the pop-up menu
                DEBUG("Displaying pop-up menu\n");
                menu_clear();
                // Menu *menu = core->create_menu(core->rom_dir)
                core_info *core = find_core_by_id(active_core);
                if (core != NULL) {
                    DEBUG("Found core_info. Displaying menu\n");
                    Menu *menu;
                    if (active_core == 6) {
                        menu = create_pcxt_menu(std::string(drv).append(core->rom_dir).c_str());
                    } else {
                        menu = create_default_menu(std::string(drv).append(core->rom_dir).c_str());
                    }
                    std::unique_ptr<Menu> menu_ptr(menu);
                    push_menu(std::move(menu_ptr));
                    menu->do_redraw();
                    menu_input_loop();
                    redraw = true;
                }
            }

            delay(20);
        }

        if (main_menu_config[choice] > 0) {
            // Load rom or core from USB drive
            struct core_info *core = NULL;
            for (int i = 0; core_info_list[i].id != 0; i++) {
                if (core_info_list[i].id == main_menu_config[choice]) {
                    core = &core_info_list[i];
                    break;
                }
            }
            if (core) {
                std::string dir = std::string(drv).append(core->rom_dir);
                menu_loadrom(dir.c_str());
            }
        } else if (main_menu_config[choice] == -1) {
            // load cores manually
            std::string dir = std::string(drv).append("cores");
            menu_loadrom(dir.c_str());
        } else if (main_menu_config[choice] == -2) {
            // Options
            menu_options();
        } 
        handle_game_action();               // combo pressed in the file browser

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

    overlay_status("Creating tasks...");
    // Create the tasks
    xTaskCreate(main_task, "main_task", MAIN_TASK_STACK_SIZE, NULL, MAIN_TASK_PRIORITY, &main_task_handle);
    xTaskCreate(uart1_rx_task, "uart1_rx_task", UART1_RX_TASK_STACK_SIZE, NULL, UART1_RX_TASK_PRIORITY, &uart1_rx_task_handle);
    
    vTaskStartScheduler();

    while (1) {
    }
}
