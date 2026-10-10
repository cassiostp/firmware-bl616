// Host sim stubs for everything the firmware links against but the sim does
// not emulate: board init, GPIO, clocks, watchdog (counted, never fires),
// USB host stack (no devices), SDH/USBH FatFs registration (the sim's ff.h
// needs none), HBN boot config, FPGA programming (records the core and
// pretends success), and system reset (asks the sim to reboot).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "board.h"
#include "bflb_device.h"
#include "bflb_gpio.h"
#include "bflb_uart.h"
#include "bflb_mtimer.h"
#include "bflb_wdg.h"
#include "bflb_irq.h"
#include "bflb_clock.h"
#include "bl616_clock.h"
#include "bl616_glb.h"
#include "bl616_hbn.h"
#include "usbh_core.h"
#include "fatfs_diskio_register.h"
#include "programmer.h"
#include "usb_gamepad.h"

#include "fpga.hpp"
#include "host.h"

namespace {
std::atomic<uint64_t> g_wdg_feeds{0};
std::atomic<uint64_t> g_wdg_last_ms{0};
uint8_t g_hbn_boot = 0;

uint64_t steady_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The link to the FPGA runs at 2 Mbaud 8N1: 5 us per byte. Pace TX to that
// rate (in bursts of at most 0.5 ms). Unpaced, the firmware's busy menu loops
// write bytes far faster than the fake FPGA thread consumes them, so the OSD
// lags behind the firmware by a growing backlog and the script runner's polls
// starve on the FPGA state lock.
void uart_pace_tx() {
    using namespace std::chrono;
    const int64_t BYTE_NS = 5000, BURST_NS = 500000, SLACK_NS = 250000;
    static const bool unpaced = getenv("TANGCORE_SIM_FAST_UART") != nullptr;
    if (unpaced)
        return;
    static std::atomic<int64_t> wire_free_ns{0}; // when the last queued byte is out
    int64_t now = (int64_t)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    int64_t prev = wire_free_ns.load(std::memory_order_relaxed);
    int64_t done;
    do {
        done = (prev > now ? prev : now) + BYTE_NS;
    } while (!wire_free_ns.compare_exchange_weak(prev, done, std::memory_order_relaxed));
    if (done - now > BURST_NS)
        std::this_thread::sleep_for(nanoseconds(done - now - SLACK_NS));
}

// Dummy devices handed out by name.
bflb_device_s g_dev_gpio, g_dev_uart1, g_dev_wdg;
} // namespace

uint64_t sim_wdg_feeds() {
    return g_wdg_feeds.load();
}
uint64_t sim_wdg_last_ms() {
    return g_wdg_last_ms.load();
}

extern "C" {

// ---- devices ----
struct bflb_device_s *bflb_device_get_by_name(const char *name) {
    if (!name)
        return nullptr;
    if (!strcmp(name, "gpio"))
        return &g_dev_gpio;
    if (!strcmp(name, "uart1"))
        return &g_dev_uart1;
    if (!strcmp(name, "watchdog"))
        return &g_dev_wdg;
    static bflb_device_s other;
    return &other;
}

// ---- board ----
void board_init(void) {}
void board_sdh_gpio_init(void) {}

// ---- gpio (unused by the sim: JTAG programming is stubbed) ----
void bflb_gpio_init(struct bflb_device_s *dev, int pin, uint32_t config) {
    (void)dev;
    (void)pin;
    (void)config;
}
void bflb_gpio_deinit(struct bflb_device_s *dev, int pin) {
    (void)dev;
    (void)pin;
}
void bflb_gpio_set(struct bflb_device_s *dev, int pin) {
    (void)dev;
    (void)pin;
}
void bflb_gpio_reset(struct bflb_device_s *dev, int pin) {
    (void)dev;
    (void)pin;
}
int bflb_gpio_read(struct bflb_device_s *dev, int pin) {
    (void)dev;
    (void)pin;
    return 0;
}
void bflb_gpio_uart_init(struct bflb_device_s *dev, int pin, int func) {
    (void)dev;
    (void)pin;
    (void)func;
}

// ---- uart: wired straight to the fake FPGA ----
void bflb_uart_init(struct bflb_device_s *dev, const struct bflb_uart_config_s *cfg) {
    (void)dev;
    (void)cfg;
}
void bflb_uart_putchar(struct bflb_device_s *dev, uint8_t b) {
    (void)dev;
    uart_pace_tx();
    fpga::tx_push(b);
}
uint8_t bflb_uart_getchar(struct bflb_device_s *dev) {
    (void)dev;
    return fpga::rx_pop();
}
int bflb_uart_rxavailable(struct bflb_device_s *dev) {
    (void)dev;
    return fpga::rx_available() ? 1 : 0;
}

// ---- timers/clocks ----
uint64_t bflb_mtimer_get_time_ms(void) {
    return sim_uptime_ms();
}
uint64_t bflb_mtimer_get_time_us(void) {
    return sim_uptime_ms() * 1000;
}
uint32_t bflb_clk_get_system_clock(int type) {
    (void)type;
    return 40000000;
}
uint32_t Clock_Peripheral_Clock_Get(int type) {
    (void)type;
    return 40000000;
}

// ---- watchdog: count feeds, never reset ----
void bflb_wdg_init(struct bflb_device_s *dev, const struct bflb_wdg_config_s *cfg) {
    (void)dev;
    (void)cfg;
}
void bflb_wdg_start(struct bflb_device_s *dev) {
    (void)dev;
}
void bflb_wdg_stop(struct bflb_device_s *dev) {
    (void)dev;
}
void bflb_wdg_reset_countervalue(struct bflb_device_s *dev) {
    (void)dev;
    g_wdg_feeds.fetch_add(1);
    g_wdg_last_ms.store(steady_ms(), std::memory_order_relaxed);
}

// ---- misc SoC ----
void bflb_irq_disable(int irq) {
    (void)irq;
}
void GLB_SW_System_Reset(void) {
    sim_request_reset();
    // The firmware never returns from here; park this thread until the
    // supervisor re-execs the process.
    for (;;)
        std::this_thread::sleep_for(std::chrono::hours(24));
}
void arch_delay_ms(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
uint8_t HBN_Get_User_Boot_Config(void) {
    return g_hbn_boot;
}
void HBN_Set_User_Boot_Config(uint8_t cfg) {
    g_hbn_boot = cfg;
}

// ---- storage/USB registration: no-ops (ff.h is host-backed) ----
void fatfs_sdh_driver_register(void) {}
void fatfs_usbh_driver_register(void) {}
void usbh_initialize(void) {}

// ---- firmware init helper (replaces utils/init.cpp: no real UART/GPIO) ----
} // extern "C"

void init_gpio_and_uart() {
    extern struct bflb_device_s *uart1_dev;
    uart1_dev = bflb_device_get_by_name("uart1");
}

// ---- USB gamepads: none attached; pads are driven by the sim ----
void usb_gamepad_init(void) {}

extern "C" {
// ---- FPGA programming: record the core, pretend success ----
bool fpga_program(const char *fname) {
    fpga::set_programmed(fname ? fname : "");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return true;
}

} // extern "C"

// Raw JTAG register pointers the firmware declares (programmer excluded, init
// stubbed): point them at dummy storage so the link always resolves.
volatile uint32_t g_sim_gpio_dummy;
volatile uint32_t *reg_gpio_tms = &g_sim_gpio_dummy;
volatile uint32_t *reg_gpio_tck = &g_sim_gpio_dummy;
volatile uint32_t *reg_gpio_tdo = &g_sim_gpio_dummy;
volatile uint32_t *reg_gpio_tdi = &g_sim_gpio_dummy;
