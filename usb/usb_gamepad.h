#pragma once

#include "FreeRTOS.h"
#include "semphr.h"
#include "usbh_core.h"
#include "usbh_hid.h"

// Start USB gamepad tasks
void usb_gamepad_init(void);
// Stop the detector and per-device client tasks (the USB debug console
// tears the host stack down). usb_gamepad_init() restarts them.
void usb_gamepad_stop(void);
// The USB host detector task's stack high-water mark in words, for the
// watchdog's periodic log snapshot (0 if it was never created).
unsigned usb_host_stack_free(void);

// state will be written here by the USB gamepad task
extern volatile uint16_t hid1_state;               // SNES-format gamepad state
extern volatile uint16_t hid2_state;               // SNES-format gamepad state
extern SemaphoreHandle_t state_mutex;              // for all global state access