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
// RX error interrupts seen by the ISR (framing/parity/line-sync status
// bits, counted once per interrupt that finds any of them set). The error
// interrupts themselves stay masked; this only samples the status bits.
uint32_t uart_rx_err_count(void);
// Raw UART_INTSTS snapshot and the ring's current fill, for link diagnostics.
uint32_t uart_rx_intsts(void);
uint32_t uart_rx_ring_level(void);
// The parser task's current frame (written per byte by uart1_rx_task, read
// only for the link-stall log line): type, declared length, payload bytes
// taken, and when the frame's 0xAA was processed (mtimer ms, 0 = no frame).
extern volatile uint8_t uart_rx_dbg_type;
extern volatile uint16_t uart_rx_dbg_len;
extern volatile uint16_t uart_rx_dbg_got;
extern volatile uint64_t uart_rx_dbg_start_ms;

// The ring and the interrupt handler's body, split from the hardware access so
// the host tests can run them against a modelled FIFO.
void uart_rx_ring_put(uint8_t ch);
bool uart_rx_ring_get(uint8_t *ch);
uint32_t uart_rx_ring_dropped(void);
void uart_rx_ring_reset(void);
// Drop bytes still in the ring. The parser task calls this when it honours
// a reset; it is the ring's only consumer.
void uart_rx_ring_discard(void);

// Ask the parser task to drop its frame state and the bytes already queued.
// The counter is what the task observes; the task itself discards the ring.
void uart_rx_request_parser_reset(void);
uint32_t uart_rx_epoch(void);
