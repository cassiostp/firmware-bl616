// Host sim shim for Bouffalo bflb_uart.h. TX bytes go to the fake FPGA,
// RX bytes come from it (see sim/fpga.hpp).
#pragma once

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

void bflb_uart_init(struct bflb_device_s *dev, const struct bflb_uart_config_s *cfg);
void bflb_uart_putchar(struct bflb_device_s *dev, uint8_t b);
uint8_t bflb_uart_getchar(struct bflb_device_s *dev);
int bflb_uart_rxavailable(struct bflb_device_s *dev);

#ifdef __cplusplus
}
#endif
