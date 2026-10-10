#pragma once

#include <strings.h>
#include <string.h>

extern "C" {
#include "bflb_gpio.h"
#include "bflb_uart.h"
#include "bflb_wdg.h"

#include <FreeRTOS.h>
#include "task.h"
#include "semphr.h"

// #include "usbh_core.h"
#include "ff.h"
}

#define DEBUG(...) dprint(__VA_ARGS__)
// #define DEBUG(...) do {} while(0)

extern struct bflb_device_s *gpio_dev;

#if defined(TANG_NANO20K)
#define GPIO_PIN_JTAG_TMS GPIO_PIN_16
#define GPIO_PIN_JTAG_TCK GPIO_PIN_10
#define GPIO_PIN_JTAG_TDI GPIO_PIN_14
#define GPIO_PIN_JTAG_TDO GPIO_PIN_12
#else
#define GPIO_PIN_JTAG_TMS GPIO_PIN_0
#define GPIO_PIN_JTAG_TCK GPIO_PIN_1
#define GPIO_PIN_JTAG_TDI GPIO_PIN_3
#define GPIO_PIN_JTAG_TDO GPIO_PIN_2
#endif

extern volatile uint32_t *reg_gpio_tms;
extern volatile uint32_t *reg_gpio_tck;
extern volatile uint32_t *reg_gpio_tdo;
extern volatile uint32_t *reg_gpio_tdi;

#define DERIVE_GPIO_OPS_OUT(GPIO_PIN_XXX)        \
    static inline void GPIO_PIN_XXX##_H(void)    \
    {                                            \
        bflb_gpio_set(gpio_dev, GPIO_PIN_XXX);   \
    }                                            \
                                                 \
    static inline void GPIO_PIN_XXX##_L(void)    \
    {                                            \
        bflb_gpio_reset(gpio_dev, GPIO_PIN_XXX); \
    }                                            \
    static inline void GPIO_PIN_XXX##_W(bool s)  \
    {                                            \
        if (s)                                   \
            GPIO_PIN_XXX##_H();                  \
        else                                     \
            GPIO_PIN_XXX##_L();                  \
    }

#define DERIVE_GPIO_OPS_IN(GPIO_PIN_XXX)               \
    static inline bool GPIO_PIN_XXX##_V(void)          \
    {                                                  \
        return bflb_gpio_read(gpio_dev, GPIO_PIN_XXX); \
    }

DERIVE_GPIO_OPS_OUT(GPIO_PIN_JTAG_TMS);
DERIVE_GPIO_OPS_OUT(GPIO_PIN_JTAG_TCK);
DERIVE_GPIO_OPS_OUT(GPIO_PIN_JTAG_TDI);
DERIVE_GPIO_OPS_IN(GPIO_PIN_JTAG_TDO);

// "sd:" or "usb:"
extern const char *drv;

static inline bool prefix(const char *pre, const char *str)
{
    return strncasecmp(pre, str, strlen(pre)) == 0;
}
// Host sim (TANGCORE_HOST): glibc's <string.h> already declares strcasestr,
// so skip this redeclaration (it fails to compile against glibc's overloads).
#ifndef TANGCORE_HOST
extern "C" char *strcasestr(const char *haystack, const char *needle);
#endif

// True if `fname` ends with `ext` (case-insensitive), e.g. has_ext("GAME.GBA", ".gba").
// Unlike strcasestr, this is a suffix match: "gba_bios.bin" does not match ".gba".
bool has_ext(const char *fname, const char *ext);
// True if `fname` ends with any of the ';'-separated extensions in `exts`
// (e.g. ".bin;.md;.gen;.smd"). An empty list matches everything.
bool has_any_ext(const char *fname, const char *exts);

// return true if core is ready. then core_id is set.
// return false if timeout after 100ms
bool get_core_status(void);
extern int joy_choice(int start_line, int len, int *active);
// Ignore the buttons held right now until each is released, so a button that
// triggered something doesn't also act on whatever comes next.
extern void suppress_held_buttons(void);
extern void send_blank_packet(void);

static inline void delay(uint32_t ms)
{
#if defined(TANG_CONSOLE60K) || defined(TANG_CONSOLE138K)
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    // compensate for 26MHz clock instead of 40MHz
    vTaskDelay(pdMS_TO_TICKS(ms*26/40));
#endif
}


extern void set_loading_state(int state);
extern void send_fbuf_data(uint16_t len);
extern void overlay_message(const char *msg, int center);

#ifndef USB_NOCACHE_RAM_SECTION
#define USB_NOCACHE_RAM_SECTION __attribute__((section(".noncacheable")))
#endif

extern bool core_running;
extern USB_NOCACHE_RAM_SECTION FIL fcore;
extern USB_NOCACHE_RAM_SECTION FIL ffloppy;
#define BLOCK_SIZE (8*1024)
extern USB_NOCACHE_RAM_SECTION BYTE __attribute__((aligned(64))) fbuf[BLOCK_SIZE];
extern bool mounted_a;

extern struct bflb_device_s *uart1_dev;
extern struct bflb_device_s *wdg_dev;

// Watchdog heartbeat: bumped by the input, file listing and data transfer
// paths, so any of them counts as "the firmware is doing something". The
// watchdog task (main.cpp) feeds the hardware watchdog only while this keeps
// moving; when a crash or deadlock stops the bumps, the chip resets.
extern volatile uint32_t heartbeat;
extern void heartbeat_bump(void);

// len: length of payload including the command (>=1)
extern void fpga_tx_header(int cmd, int len);
extern void fpga_tx_byte(uint8_t b);

extern uint32_t get_file_size(const char *fname);

// Send a romdata packet to core of len bytes in `fbuf`
extern void send_fbuf_data(uint16_t len);
extern void send_blank_packet(void);

extern SemaphoreHandle_t state_mutex;              
extern volatile uint16_t joy1_state;
extern volatile uint16_t joy2_state;
extern volatile uint16_t hid1_state;
extern volatile uint16_t hid2_state;
extern volatile int16_t core_id;
extern volatile uint8_t key_buf[4];     // ascii keys when overlay is on, non-zero if key is pressed
                                        // read should take all bytes and clear buffer

extern void get_joypad_states(uint16_t *joy1, uint16_t *joy2, uint16_t *hid1, uint16_t *hid2);
extern int16_t get_core_id(void);
extern uint32_t get_core_config(void);
extern void set_core_config(uint32_t config);
extern void forget_core_config(void);     // a new bitstream: our copy is 0 again
extern uint32_t get_video_config(void);
extern void set_video_config(uint32_t config);
extern void forget_video_config(void);    // same: the new core starts with video_config = 0

// core_config option bits shared by all game cores. Bit 16 enables the
// optional scanline effect, bit 17 pauses the game while the game menu is
// open, bits 19:18 set how dark the scanlines are (25/50/75/100 %), bit 20
// makes them thick, bit 21 keeps the picture full size with the lines at a
// fixed pitch (instead of an integer scale), and bit 22 mutes the pads.
// Every core defaults them to 0.
#define CORE_CFG_SCANLINES (1u << 16)
#define CORE_CFG_MENU_PAUSE (1u << 17)
#define CORE_CFG_SCANLINE_DARK_SHIFT 18
#define CORE_CFG_SCANLINE_THICK (1u << 20)
#define CORE_CFG_SCANLINE_FULL (1u << 21)
#define CORE_CFG_MUTE_PADS (1u << 22)

// video_config option bits (frame 0x13) shared by all game cores. Bits 2:0
// add -4..+3 steps of 16 to every channel (brightness), bits 5:3 scale
// around 128 (contrast), bits 8:6 blend towards luma (saturation, -4 is
// grey), bits 10:9 pick a gamma table (0 off, 1 darker, 2 brighter, 3 CRT),
// bits 12:11 pick the CRT mask (0 off, 1 grille, 2 slot, 3 dot), bits 14:13
// set how much the mask dims (1/4, 3/8, 1/2, 5/8), bit 15 turns the LCD grid
// on, and bits 17:16 set how much the grid dims (1/8, 1/4, 3/8, 1/2).
// Bits 19:18 are reserved for phase-2 smoothing. Every core defaults them
// to 0, and all-zero video_config leaves the picture unchanged.
#define VIDEO_CFG_BRIGHT_SHIFT 0
#define VIDEO_CFG_CONTRAST_SHIFT 3
#define VIDEO_CFG_SATUR_SHIFT 6
#define VIDEO_CFG_GAMMA_SHIFT 9
#define VIDEO_CFG_MASK_SHIFT 11
#define VIDEO_CFG_MASK_STRENGTH_SHIFT 13
#define VIDEO_CFG_LCD_GRID (1u << 15)
#define VIDEO_CFG_GRID_STRENGTH_SHIFT 16

extern const char *cstr_find_ignore_case(const char *str, const char *substr);
