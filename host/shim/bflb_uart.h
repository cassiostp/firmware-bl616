// Host sim shim for Bouffalo bflb_uart.h. TX bytes go to the fake FPGA,
// RX bytes come from it (see sim/fpga.hpp).
#pragma once

#include <stdbool.h>

#include "bflb_device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UART_DIRECTION_TXRX 1
#define UART_DATA_BITS_8 8
#define UART_STOP_BITS_1 1
#define UART_PARITY_NONE 0
#define UART_LSB_FIRST 0

struct bflb_uart_config_s {
    uint32_t baudrate;
    int direction;
    int data_bits;
    int stop_bits;
    int parity;
    int bit_order;
    int flow_ctrl;
    int tx_fifo_threshold;
    int rx_fifo_threshold;
};

#define UART_INTSTS_RX_FIFO (1 << 3)
#define UART_INTSTS_RTO (1 << 4)
#define UART_INTSTS_PCE (1 << 5)
#define UART_INTSTS_RX_FER (1 << 7)
#define UART_INTSTS_RX_LSE (1 << 8)
#define UART_INTCLR_RTO (1 << 4)

void bflb_uart_init(struct bflb_device_s *dev, const struct bflb_uart_config_s *cfg);
void bflb_uart_putchar(struct bflb_device_s *dev, uint8_t b);
uint8_t bflb_uart_getchar(struct bflb_device_s *dev);
int bflb_uart_rxavailable(struct bflb_device_s *dev);
void bflb_uart_rxint_mask(struct bflb_device_s *dev, bool mask);
uint32_t bflb_uart_get_intstatus(struct bflb_device_s *dev);
void bflb_uart_int_clear(struct bflb_device_s *dev, uint32_t int_clear);

#ifdef __cplusplus
}
#endif
