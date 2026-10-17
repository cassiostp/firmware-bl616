// UART1 receive: an interrupt drains the hardware FIFO into a ring buffer, and
// uart1_rx_task reads the ring.
//
// WHY: the BL616 UART's receive FIFO holds 32 bytes, 160 us of the FPGA's
// 2 Mbaud stream. uart1_rx_task polls and sleeps a tick (1 ms) between polls,
// so any frame longer than 32 bytes that starts while it sleeps loses its tail
// -- and a battery-save block (0x0A) is 515 bytes. A dump that gets no complete
// block times out (core/saves.cpp), and no save ever reaches the card. The
// interrupt runs within a few us of the FIFO filling to its threshold, so the
// task can sleep as long as the ring is deep (2048 bytes = 10 ms of stream).
//
// The ring is single-producer (the interrupt), single-consumer (the task) on
// one core: no lock, only ordering of the index updates.
#include "uart_rx.h"

#include "utils.h"

extern "C" {
#include "bflb_irq.h"
}

#define RING_SIZE 2048                  // power of two
#define RING_MASK (RING_SIZE - 1)

static uint8_t ring[RING_SIZE];
static volatile uint32_t ring_head;     // next slot the interrupt fills
static volatile uint32_t ring_tail;     // next slot the task reads
static volatile uint32_t ring_lost;     // bytes dropped because the ring was full
static volatile uint32_t ring_errs;     // interrupts that saw an RX error bit

void uart_rx_ring_reset(void) {
    ring_head = 0;
    ring_tail = 0;
    ring_lost = 0;
}

void uart_rx_ring_put(uint8_t ch) {
    uint32_t h = ring_head;
    if (h - __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE) >= RING_SIZE) {
        ring_lost++;
        return;
    }
    ring[h & RING_MASK] = ch;
    __atomic_store_n(&ring_head, h + 1, __ATOMIC_RELEASE);
}

bool uart_rx_ring_get(uint8_t *ch) {
    uint32_t t = ring_tail;
    if (t == __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE))
        return false;
    *ch = ring[t & RING_MASK];
    __atomic_store_n(&ring_tail, t + 1, __ATOMIC_RELEASE);
    return true;
}

uint32_t uart_rx_ring_dropped(void) {
    return ring_lost;
}

uint32_t uart_rx_err_count(void) {
    return ring_errs;
}

uint32_t uart_rx_intsts(void) {
    return bflb_uart_get_intstatus(uart1_dev);
}

uint32_t uart_rx_ring_level(void) {
    return __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE) -
           __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE);
}

bool uart_rx_get(uint8_t *ch) {
    return uart_rx_ring_get(ch);
}

// UART_FIFO_CONFIG_0 (0x80) bit 6: the receive FIFO overflowed since it was
// last cleared (SDK drivers/lhal/include/hardware/uart_reg.h). Sticky, so one
// read tells whether the hardware lost a byte at any time since boot.
bool uart_rx_fifo_overflowed(void) {
#ifndef TANGCORE_HOST
    return uart1_dev && (*(volatile uint32_t *)(uart1_dev->reg_base + 0x80) & (1u << 6));
#else
    return false;
#endif
}

// The interrupt: the receive FIFO reached its threshold, or went quiet with
// bytes in it (the receiver timeout, which must be cleared by hand).
extern "C" void uart_rx_isr(int irq, void *arg) {
    (void)irq;
    (void)arg;
    uint32_t st = bflb_uart_get_intstatus(uart1_dev);
    if (st & UART_INTSTS_RTO)
        bflb_uart_int_clear(uart1_dev, UART_INTCLR_RTO);
    // RX error bits (parity, framing, line-sync): their interrupts stay
    // masked (bflb_uart_init masks everything, uart_rx_init unmasks only
    // FIFO+RTO), so a latched error can neither storm nor block receive
    // interrupts. Count and clear them so link diagnostics can tell a
    // framing-error burst from a silent FPGA.
    uint32_t err = st & (UART_INTSTS_PCE | UART_INTSTS_RX_FER |
                         UART_INTSTS_RX_LSE);
    if (err) {
        ring_errs++;
        bflb_uart_int_clear(uart1_dev, err);
    }
    while (bflb_uart_rxavailable(uart1_dev))
        uart_rx_ring_put((uint8_t)bflb_uart_getchar(uart1_dev));
}

void uart_rx_init(void) {
    uart_rx_ring_reset();
    bflb_uart_rxint_mask(uart1_dev, false);      // unmask the FIFO and timeout interrupts
    bflb_irq_attach(uart1_dev->irq_num, uart_rx_isr, NULL);
    bflb_irq_enable(uart1_dev->irq_num);
}
