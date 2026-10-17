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
#include <mutex>
#include <thread>

#include "board.h"
#include "bflb_device.h"
#include "bflb_gpio.h"
#include "bflb_uart.h"
#include "bflb_mtimer.h"
#include "bflb_rtc.h"
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
#include "fastload.h"
#include "uart_rx.h"
#include "usb_console.h"
#include "dbg_console.h"

#include "fpga.hpp"
#include "host.h"
#include "sim_time.hpp"

namespace {
std::atomic<bool> g_uart_rxint_masked{true};
std::mutex g_isr_m;
int g_irq_num = 0;
irq_callback g_irq_isr = nullptr;
void *g_irq_arg = nullptr;
std::atomic<uint64_t> g_wdg_feeds{0};
std::atomic<uint64_t> g_wdg_last_ms{0};
uint8_t g_hbn_boot = 0;

uint64_t steady_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
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
void bflb_uart_rxint_mask(struct bflb_device_s *dev, bool mask) {
    (void)dev;
    g_uart_rxint_masked.store(mask);
}
uint32_t bflb_uart_get_intstatus(struct bflb_device_s *dev) {
    (void)dev;
    return fpga::rx_available() ? UART_INTSTS_RX_FIFO : 0;
}
void bflb_uart_int_clear(struct bflb_device_s *dev, uint32_t int_clear) {
    (void)dev;
    (void)int_clear;
}
int bflb_uart_feature_control(struct bflb_device_s *dev, int cmd, size_t arg) {
    (void)dev;
    if (cmd == UART_CMD_SET_BAUD_RATE)
        fpga::set_baud((uint32_t)arg);
    return 0; // CLR_RX_FIFO and the rest are no-ops: the sim has no FIFO
}
int bflb_uart_wait_tx_done(struct bflb_device_s *dev) {
    (void)dev;
    return 0; // putchar delivers synchronously in the sim
}
bool bflb_uart_txempty(struct bflb_device_s *dev) {
    (void)dev;
    return true;
}

// ---- timers/clocks: the virtual simulation clock ----
// spin_poll bounds firmware polling loops (the menu input loop reads the
// clock every pass with no delay): after many reads without a sim block
// it sleeps until the next scheduler chunk so time can advance.
uint64_t bflb_mtimer_get_time_ms(void) {
    sim::spin_poll();
    return sim::now_ms();
}
uint64_t bflb_mtimer_get_time_us(void) {
    sim::spin_poll();
    return sim::now_us();
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
// The interrupt controller, for the one interrupt the firmware uses here (the
// UART1 receive interrupt, utils/uart_rx.cpp). The backends call
// fpga::rx_notify() when they queue bytes for the MCU; the hook runs the
// registered handler, which drains them into the firmware's ring buffer exactly
// as on the chip, in the same sim step the bytes arrived (the handler is
// serialized, like one interrupt line).
void run_uart_isr() {
    std::lock_guard<std::mutex> lk(g_isr_m);
    if (!g_uart_rxint_masked.load() && g_irq_isr)
        g_irq_isr(g_irq_num, g_irq_arg);
}
void bflb_irq_attach(int irq, irq_callback isr, void *arg) {
    g_irq_num = irq;
    g_irq_isr = isr;
    g_irq_arg = arg;
}
void bflb_irq_enable(int irq) {
    (void)irq;
    fpga::set_rx_hook(run_uart_isr);
    run_uart_isr();                     // bytes that arrived before it was enabled
}
void bflb_irq_disable(int irq) {
    (void)irq;
}
void GLB_SW_System_Reset(void) {
    sim_request_reset();
    // The firmware never returns from here; park this thread as blocked
    // until the supervisor re-execs the process (a wall sleep would look
    // runnable and freeze the sim clock the supervisor is waiting on).
    sim::park();
}
void arch_delay_ms(uint32_t ms) {
    sim::advance_by((uint64_t)ms * sim::TICKS_PER_MS);
}
uint8_t HBN_Get_User_Boot_Config(void) {
    return g_hbn_boot;
}
void HBN_Set_User_Boot_Config(uint8_t cfg) {
    g_hbn_boot = cfg;
}

// ---- HBN RTC counter: 32 kHz ticks since "power-on". The base survives a
// re-exec only when sim_hbn_preserve(true) exported it (MODE reset); a
// power cycle starts again from 0, like the chip.
static uint64_t hbn_base_ticks(void) {
    static uint64_t base = [] {
        const char *e = getenv("TANGCORE_SIM_HBN_TICKS");
        return e ? strtoull(e, nullptr, 10) : 0;
    }();
    return base;
}
int HBN_Enable_RTC_Counter(void) {
    (void)hbn_base_ticks();             // enable bit only: the counter keeps running
    return 0;
}
uint64_t bflb_rtc_get_time(struct bflb_device_s *dev) {
    (void)dev;
    return hbn_base_ticks() + sim::now_ms() * 32768ull / 1000ull;
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
    uart_rx_init();
}

// core/fastload.cpp (TANGCORE_HOST): the MCU switched baud; the backend's
// bit-level serialiser (RTL) or rate tracker (fake) follows.
void sim_uart_set_baud(uint32_t baud) {
    fpga::set_baud(baud);
}

// ---- USB gamepads: none attached; pads are driven by the sim ----
void usb_gamepad_init(void) {}
unsigned usb_host_stack_free(void) { return 0; }

extern "C" {
// ---- FPGA programming: record the core, pretend success ----
bool fpga_program(const char *fname) {
    fpga::set_programmed(fname ? fname : "");
    sim::advance_by(50 * sim::TICKS_PER_MS); // JTAG took a moment
    return true;
}

} // extern "C"

void sim_hbn_preserve(bool keep) {
    if (keep) {
        char buf[32];
        snprintf(buf, sizeof buf, "%llu",
                 (unsigned long long)bflb_rtc_get_time(nullptr));
        setenv("TANGCORE_SIM_HBN_TICKS", buf, 1);
    } else {
        unsetenv("TANGCORE_SIM_HBN_TICKS");
    }
}

// Raw JTAG register pointers the firmware declares (programmer excluded, init
// stubbed): point them at dummy storage so the link always resolves.
volatile uint32_t g_sim_gpio_dummy;
volatile uint32_t *reg_gpio_tms = &g_sim_gpio_dummy;
volatile uint32_t *reg_gpio_tck = &g_sim_gpio_dummy;
volatile uint32_t *reg_gpio_tdo = &g_sim_gpio_dummy;
volatile uint32_t *reg_gpio_tdi = &g_sim_gpio_dummy;

// ---- USB debug console: no USB device in the sim; the on/off flag and the
// command core are real, so the Options toggle and MODE path run as on HW.
namespace {
bool g_usb_console_on = false;
bool sim_console_write(const char *, size_t n) { return true; (void)n; }
} // namespace

void usb_console_init(void) {
    dbg_console_init(nullptr);
    dbg_console_set_transport(sim_console_write);
}

bool usb_console_set_enabled(bool on) {
    extern const char *drv;
    if (on && drv && !strcmp(drv, "usb:"))
        return false;
    g_usb_console_on = on;
    dbg_console_set_active(on);
    return true;
}

bool usb_console_enabled(void) {
    return g_usb_console_on;
}

extern "C" int GLB_Get_Reset_Reason(GLB_RESET_RECORD_Type *reason) {
    if (reason) {
        memset(reason, 0, sizeof *reason);
        reason->reset_recorder_ana_powb = 1;
    }
    return 0;
}
