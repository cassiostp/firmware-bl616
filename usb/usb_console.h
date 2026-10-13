// USB debug console: switches the BL616 USB port from host to a CDC ACM
// device so a PC plugged in after boot can watch and query the running
// firmware. See utils/dbg_log.h and utils/dbg_console.h for the log ring
// and the command set. Plain header (no CherryUSB types) so the host sim
// can include it and stub the two functions.
#pragma once

#include <stdbool.h>

// Register the console's platform callbacks and start its task. Call once
// from main(), after dbg_log_init().
void usb_console_init(void);

// Turn the console on/off (the Options menu). Turning it on tears down the
// USB host stack first: BL616-side pads (diag "H") and the USB drive stop
// working until it is turned off again. Returns false (and changes
// nothing) when the firmware booted from the USB drive (drv == "usb:").
// Off by default; never remembered across power cycles.
bool usb_console_set_enabled(bool on);
bool usb_console_enabled(void);
