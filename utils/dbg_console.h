// Line-command console over the debug log ring. Transport-agnostic: the
// USB CDC task (usb/usb_console.cpp) feeds received bytes in and provides
// a non-blocking write; the host tests do the same with a fake.
//
// dbg_console_poll() runs in the console task: it streams new log lines,
// prints the per-second sniff summaries and times out confirmations.
// Output that the transport cannot take is dropped and counted, never
// waited for, so a PC that stops reading cannot stall the firmware.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Emit one formatted line (or fragment) to the console user.
typedef void (*dbg_emit_fn)(const char *fmt, ...);

// What the console cannot know itself; the firmware registers these
// (usb/usb_console.cpp), the host tests register fakes. Any may be NULL.
struct dbg_platform {
    void (*cmd_status)(dbg_emit_fn emit);   // diag rows, core, uptime, heap
    void (*cmd_tasks)(dbg_emit_fn emit);    // per-task state + stack high water
    void (*cmd_saves)(dbg_emit_fn emit);    // save counters
    void (*cmd_link)(dbg_emit_fn emit);     // UART link counters
    void (*do_reboot)(void);                // clean MCU restart; does not return
    void (*do_wdtest)(void);                // stall the heartbeat on purpose (watchdog recovery test)
};

void dbg_console_init(const struct dbg_platform *platform);
// Non-blocking transport write: returns false if the bytes cannot be taken
// now (no reader, buffer full); the console then drops and counts them.
void dbg_console_set_transport(bool (*write)(const char *data, size_t n));

void dbg_console_set_active(bool on);
bool dbg_console_active(void);
void dbg_console_feed(const char *data, size_t n);  // bytes from the PC
void dbg_console_poll(void);                        // call every ~20 ms
uint32_t dbg_console_dropped(void);                 // output bytes dropped

// MCU<->FPGA frame sniffing: cheap counters only, safe anywhere a volatile
// increment is (task code; the UART ISR does not call these).
void dbg_sniff_note_tx(uint8_t cmd);
void dbg_sniff_note_rx(uint8_t type);

#ifdef __cplusplus
}
#endif
