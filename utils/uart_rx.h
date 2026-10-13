// UART1 receive path: the FPGA's bytes, drained from the hardware FIFO by an
// interrupt into a ring buffer. See uart_rx.cpp.
#pragma once

#include <stdint.h>

// Call once, right after bflb_uart_init(uart1_dev, ...).
void uart_rx_init(void);
// Next byte the FPGA sent. False if none is waiting.
bool uart_rx_get(uint8_t *ch);
// The 32-byte hardware FIFO overflowed at some point since boot (the
// interrupt could not keep up: it was blocked for longer than 160 us).
bool uart_rx_fifo_overflowed(void);
// " P" in the A/B build that polls the FIFO (UART_RX_POLL), else "".
const char *uart_rx_mode_tag(void);

// The ring and the interrupt handler's body, split from the hardware access so
// the host tests can run them against a modelled FIFO.
void uart_rx_ring_put(uint8_t ch);
bool uart_rx_ring_get(uint8_t *ch);
uint32_t uart_rx_ring_dropped(void);
void uart_rx_ring_reset(void);
