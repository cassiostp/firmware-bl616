// USB CDC ACM debug console (device side) + the host<->device swap.
//
// The BL616 has one USB controller (EHCI host + device at 0x20072000, IRQ
// 37). Normal firmware runs it as a host for the USB drive and pads. The
// console stops the host stack, flips the PDS role bit back to B-device
// and starts CherryUSB's device stack as CDC ACM, so a PC plugged into
// the BL616 port AFTER boot sees /dev/ttyACM*. (A PC present at power-up
// still parks the board in Sipeed's debugger mode; that is decided before
// this firmware runs and cannot be changed here.)
//
// All console output is drop-rather-than-block: transport_write() copies
// into a small ring and reports false when full or when no terminal holds
// DTR, and dbg_console counts the loss.
#include <stdio.h>
#include <string.h>
#include <malloc.h>

extern "C" {
#include <FreeRTOS.h>
#include "task.h"
#include "portable.h"
#include "bflb_irq.h"
#include "bflb_mtimer.h"
#include "usbh_core.h"
#include "usbd_core.h"
#include "usbd_cdc.h"
#include "ff.h"
}

#include "usb_console.h"
#include "usb_gamepad.h"
#include "dbg_log.h"
#include "dbg_console.h"
#include "utils.h"
#include "uart_rx.h"
#include "saves.h"
#include "cores.h"
#include "game_controls.h"

#define CDC_IN_EP  0x81
#define CDC_OUT_EP 0x02
#define CDC_INT_EP 0x83
// pid.codes test VID/PID (1209:0001), for prototyping only. Linux binds
// cdc_acm by class, so no driver is needed either way.
#define USBD_VID 0x1209
#define USBD_PID 0x0001
#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)
#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

#define USB_IRQ 37
// PDS/OTG role bits, same registers as usb_back_to_power_on_state()
// (ui/game_controls.cpp).
#define TC_USB_BASE 0x20072000
#define USB_OTG_CSR (TC_USB_BASE + 0x80)
#define USB_A_BUS_REQ_HOV (1 << 4)
#define USB_A_BUS_DROP_HOV (1 << 5)
#define PDS_USB_CTL 0x2000e500
#define PDS_USB_IDDIG (1 << 5)

extern int16_t active_core;             // main.cpp

extern "C" __attribute__((weak)) struct mallinfo mallinfo(void);

static const uint8_t cdc_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, 100),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02),
    USB_LANGID_INIT(1033),
    // string1 "TangCore"
    0x12, USB_DESCRIPTOR_TYPE_STRING,
    'T', 0, 'a', 0, 'n', 0, 'g', 0, 'C', 0, 'o', 0, 'r', 0, 'e', 0,
    // string2 "TangCore Debug Console"
    0x2E, USB_DESCRIPTOR_TYPE_STRING,
    'T', 0, 'a', 0, 'n', 0, 'g', 0, 'C', 0, 'o', 0, 'r', 0, 'e', 0,
    ' ', 0, 'D', 0, 'e', 0, 'b', 0, 'u', 0, 'g', 0, ' ', 0,
    'C', 0, 'o', 0, 'n', 0, 's', 0, 'o', 0, 'l', 0, 'e', 0,
    // string3 "TCDBG001"
    0x12, USB_DESCRIPTOR_TYPE_STRING,
    'T', 0, 'C', 0, 'D', 0, 'B', 0, 'G', 0, '0', 0, '0', 0, '1', 0,
#ifdef CONFIG_USB_HS
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02, 0x02, 0x02, 0x01, 0x40, 0x01, 0x00,
#endif
    0x00
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t cdc_read_buf[CDC_MAX_MPS];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t cdc_write_buf[CDC_MAX_MPS];

static volatile bool ep_tx_busy;
static volatile bool dtr_on;
static volatile bool configured;

// PC -> firmware bytes (IRQ producer, console task consumer).
#define RX_RING_SIZE 512
static uint8_t rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head, rx_tail;

// Firmware -> PC bytes waiting for the IN endpoint.
#define TX_RING_SIZE 4096
static char tx_ring[TX_RING_SIZE];
static uint32_t tx_head, tx_tail;       // guarded by critical sections

extern "C" void usbd_event_handler(uint8_t event) {
    if (event == USBD_EVENT_CONFIGURED) {
        configured = true;
        usbd_ep_start_read(CDC_OUT_EP, cdc_read_buf, sizeof cdc_read_buf);
    } else if (event == USBD_EVENT_DISCONNECTED || event == USBD_EVENT_RESET) {
        configured = false;
        dtr_on = false;
        ep_tx_busy = false;
    }
}

extern "C" void usbd_cdc_acm_set_dtr(uint8_t intf, bool dtr) {
    (void)intf;
    dtr_on = dtr;
}

static void cdc_bulk_out(uint8_t ep, uint32_t nbytes) {
    (void)ep;
    for (uint32_t i = 0; i < nbytes; i++) {
        uint32_t h = rx_head;
        if (h - __atomic_load_n(&rx_tail, __ATOMIC_ACQUIRE) < RX_RING_SIZE) {
            rx_ring[h % RX_RING_SIZE] = cdc_read_buf[i];
            __atomic_store_n(&rx_head, h + 1, __ATOMIC_RELEASE);
        }                               // full: drop the keystroke
    }
    usbd_ep_start_read(CDC_OUT_EP, cdc_read_buf, sizeof cdc_read_buf);
}

static void cdc_bulk_in(uint8_t ep, uint32_t nbytes) {
    (void)ep;
    if ((nbytes % CDC_MAX_MPS) == 0 && nbytes)
        usbd_ep_start_write(CDC_IN_EP, NULL, 0);    // ZLP
    else
        ep_tx_busy = false;
}

static struct usbd_endpoint cdc_out_ep = { CDC_OUT_EP, cdc_bulk_out };
static struct usbd_endpoint cdc_in_ep = { CDC_IN_EP, cdc_bulk_in };
static struct usbd_interface intf0, intf1;

static bool transport_write(const char *data, size_t n) {
    if (!usb_console_enabled() || !configured || !dtr_on)
        return false;
    bool ok = true;
    taskENTER_CRITICAL();
    if (tx_head - tx_tail + n > TX_RING_SIZE) {
        ok = false;
    } else {
        for (size_t i = 0; i < n; i++)
            tx_ring[tx_head++ % TX_RING_SIZE] = data[i];
    }
    taskEXIT_CRITICAL();
    return ok;
}

static void pump_tx(void) {
    if (!configured || !dtr_on || ep_tx_busy)
        return;
    taskENTER_CRITICAL();
    uint32_t avail = tx_head - tx_tail;
    if (!avail) {
        taskEXIT_CRITICAL();
        return;
    }
    if (avail > CDC_MAX_MPS)
        avail = CDC_MAX_MPS;
    for (uint32_t i = 0; i < avail; i++)
        cdc_write_buf[i] = (uint8_t)tx_ring[tx_tail++ % TX_RING_SIZE];
    taskEXIT_CRITICAL();
    ep_tx_busy = true;
    usbd_ep_start_write(CDC_IN_EP, cdc_write_buf, avail);
}

/////////////////////////////////////////////////////////////////////////////////
// dbg_platform: the command bodies that need firmware state

static void pf_status(dbg_emit_fn emit) {
    uint16_t joy1, joy2, hid1, hid2;
    get_joypad_states(&joy1, &joy2, &hid1, &hid2);
    struct core_info *c = find_core_by_id(active_core > 0 ? (uint16_t)active_core : 0);
    uint64_t up = bflb_mtimer_get_time_ms();
    emit("core: %d%s%s%s uptime %lus heartbeat %lu longest silence %ums\n",
         active_core, c ? " (" : "", c ? c->display_name : "", c ? ")" : "",
         (unsigned long)(up / 1000), (unsigned long)heartbeat,
         (unsigned)mode_silence_ms());
    char sv[40];
    saves_diag_text(sv, sizeof sv);
    emit("diag0: J%03x%03x H%03x%03x c%d\n", joy1 & 0xfff, joy2 & 0xfff,
         hid1 & 0xfff, hid2 & 0xfff, active_core);
    emit("diag1: %s\n", sv);
    // The SDK builds FreeRTOS heap_3 (malloc-backed): no
    // xPortGetFreeHeapSize. mallinfo may or may not exist in this
    // newlib build, hence the weak reference.
    if (&mallinfo) {
        struct mallinfo mi = mallinfo();
        emit("heap: arena %d, free %d, in use %d\n", mi.arena, mi.fordblks, mi.uordblks);
    } else {
        emit("heap: n/a (heap_3, no mallinfo in this libc)\n");
    }
    emit("usb console: %s, configured %d, dtr %d, drive %s\n",
         usb_console_enabled() ? "on" : "off", configured ? 1 : 0,
         dtr_on ? 1 : 0, drv);
}

static const char *task_state_name(eTaskState s) {
    switch (s) {
    case eRunning: return "run";
    case eReady: return "rdy";
    case eBlocked: return "blk";
    case eSuspended: return "sus";
    case eDeleted: return "del";
    default: return "?";
    }
}

static void pf_tasks(dbg_emit_fn emit) {
    UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *st = (TaskStatus_t *)pvPortMalloc(count * sizeof *st);
    if (!st) {
        emit("tasks: out of memory\n");
        return;
    }
    count = uxTaskGetSystemState(st, count, NULL);
    emit("%-14s %-4s %3s %8s\n", "name", "st", "pri", "stkfree");
    for (UBaseType_t i = 0; i < count; i++)
        emit("%-14s %-4s %3lu %8lu\n", st[i].pcTaskName,
             task_state_name(st[i].eCurrentState),
             (unsigned long)st[i].uxCurrentPriority,
             (unsigned long)st[i].usStackHighWaterMark);
    vPortFree(st);
    emit("stkfree: stack high-water mark, words still free (0 = overflowed)\n");
}

static void pf_saves(dbg_emit_fn emit) {
    char diag[48], status[48];
    saves_diag_text(diag, sizeof diag);
    saves_status_text(status, sizeof status);
    emit("%s\n", diag);
    emit("w=wrote s=same/current f=failed n=no-reply blocks b=bad frames, O=FIFO overflow, C=save channel seen\n");
    emit("current game: %s\n", status[0] ? status : "(no battery-save game)");
    emit("save in flight: %s\n", saves_busy() ? "yes" : "no");
}

static void pf_link(dbg_emit_fn emit) {
    emit("uart rx ring dropped: %lu bytes\n", (unsigned long)uart_rx_ring_dropped());
    emit("uart hw fifo overflow: %s\n", uart_rx_fifo_overflowed() ? "YES (sticky)" : "no");
    emit("fpga core_id (last reply): %d\n", (int)core_id);
    emit("longest fpga silence: %u ms\n", (unsigned)mode_silence_ms());
}

static void pf_reboot(void) {
    // Runs on the console task, which is also the TX pump: flush the
    // "rebooting" note out before resetting.
    for (int i = 0; i < 25; i++) {
        pump_tx();
        if (!ep_tx_busy && tx_head == tx_tail)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    tc_restart_mcu(false);
}

static void pf_wdtest(void) {
    dbg_wdtest_stall = true;            // heartbeat_bump + watchdog task stop feeding
}

static const struct dbg_platform platform = {
    pf_status, pf_tasks, pf_saves, pf_link, pf_reboot, pf_wdtest,
};

/////////////////////////////////////////////////////////////////////////////////
// Console task: pumps both directions and turns UART drop counters into
// log lines (the ISR itself must not log).

static void console_task(void *arg) {
    (void)arg;
    uint32_t seen_dropped = uart_rx_ring_dropped();
    bool seen_overflow = false;
    for (;;) {
        uint32_t dropped = uart_rx_ring_dropped();
        if (dropped != seen_dropped) {
            dbg_log("uart rx ring dropped %lu bytes (total %lu)",
                    (unsigned long)(dropped - seen_dropped), (unsigned long)dropped);
            seen_dropped = dropped;
        }
        if (!seen_overflow && uart_rx_fifo_overflowed()) {
            seen_overflow = true;
            dbg_log("uart hw fifo overflowed (first time)");
        }

        // PC input -> command parser
        char in[64];
        size_t n = 0;
        uint32_t t = rx_tail;
        while (t != __atomic_load_n(&rx_head, __ATOMIC_ACQUIRE) && n < sizeof in) {
            in[n++] = (char)rx_ring[t % RX_RING_SIZE];
            __atomic_store_n(&rx_tail, ++t, __ATOMIC_RELEASE);
        }
        if (n)
            dbg_console_feed(in, n);

        dbg_console_poll();
        pump_tx();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void usb_console_init(void) {
    dbg_console_init(&platform);
    dbg_console_set_transport(transport_write);
    if (xTaskCreate(console_task, "usb_console", 1536, NULL, 2, NULL) != pdPASS)
        dbg_log("usb console: task not created (heap?), console unavailable");
}

/////////////////////////////////////////////////////////////////////////////////
// The swap

static bool enabled;

bool usb_console_enabled(void) {
    return enabled;
}

static inline void reg_set(uint32_t addr, uint32_t bits, bool on) {
    volatile uint32_t *r = (volatile uint32_t *)addr;
    *r = on ? (*r | bits) : (*r & ~bits);
}

bool usb_console_set_enabled(bool on) {
    if (on == enabled)
        return true;
    if (on) {
        if (!strcmp(drv, "usb:"))
            return false;               // the running drive IS the USB host stack
        dbg_log("usb console: switching USB host -> device (CDC ACM)");
        f_mount(NULL, "usb:", 0);       // no-op unless something mounted it
        usb_gamepad_stop();
        if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
            hid1_state = 0;
            hid2_state = 0;
            memset((void *)key_buf, 0, sizeof key_buf);
            xSemaphoreGive(state_mutex);
        }
        usbh_deinitialize();
        bflb_irq_disable(USB_IRQ);
        bflb_irq_detach(USB_IRQ);
        // Host drove VBUS and cleared IDDIG (device-A). Device mode wants
        // the bus dropped and IDDIG set (B-device); usb_dc_init does not
        // touch IDDIG, so set it here. The PC supplies VBUS as host.
        reg_set(USB_OTG_CSR, USB_A_BUS_REQ_HOV, false);
        reg_set(USB_OTG_CSR, USB_A_BUS_DROP_HOV, true);
        reg_set(PDS_USB_CTL, PDS_USB_IDDIG, true);
        vTaskDelay(pdMS_TO_TICKS(50));  // let host VBUS decay
        // usbd_desc_register() memsets the core state, so the full
        // sequence is safe to repeat on every enable.
        usbd_desc_register(cdc_descriptor);
        usbd_add_interface(usbd_cdc_acm_init_intf(&intf0));
        usbd_add_interface(usbd_cdc_acm_init_intf(&intf1));
        usbd_add_endpoint(&cdc_out_ep);
        usbd_add_endpoint(&cdc_in_ep);
        configured = false;
        dtr_on = false;
        ep_tx_busy = false;
        taskENTER_CRITICAL();           // no stale bytes from a prior session
        tx_head = tx_tail = 0;
        taskEXIT_CRITICAL();
        __atomic_store_n(&rx_head, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&rx_tail, 0, __ATOMIC_RELEASE);
        int r = usbd_initialize();
        if (r != 0) {
            dbg_log("usb console: usbd_initialize failed (%d), restoring host", r);
            bflb_irq_disable(USB_IRQ);
            bflb_irq_detach(USB_IRQ);
            usbh_initialize();
            usb_gamepad_init();
            return false;
        }
        enabled = true;
        dbg_console_set_active(true);
        dbg_log("usb console: device up, waiting for PC");
        return true;
    }

    dbg_log("usb console: switching USB device -> host");
    dbg_console_set_active(false);
    enabled = false;
    usbd_deinitialize();
    bflb_irq_disable(USB_IRQ);
    bflb_irq_detach(USB_IRQ);
    configured = false;
    dtr_on = false;
    ep_tx_busy = false;
    taskENTER_CRITICAL();
    tx_head = tx_tail = 0;
    taskEXIT_CRITICAL();
    __atomic_store_n(&rx_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rx_tail, 0, __ATOMIC_RELEASE);
    // The FATFS USBH disk driver stays registered from boot; only the
    // host stack and the gamepad detector need restarting.
    int r = usbh_initialize();
    usb_gamepad_init();
    dbg_log("usb console: host restored (usbh_initialize=%d)", r);
    return true;
}
