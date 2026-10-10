// Host sim shim for Bouffalo bflb_gpio.h. Only declarations; the sim never
// touches real GPIO pins (JTAG programming is stubbed, UART pins are virtual).
#pragma once

#include "bflb_device.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GPIO_PIN_0 = 0,
    GPIO_PIN_1,
    GPIO_PIN_2,
    GPIO_PIN_3,
    GPIO_PIN_10 = 10,
    GPIO_PIN_11,
    GPIO_PIN_12,
    GPIO_PIN_13,
    GPIO_PIN_14,
    GPIO_PIN_15,
    GPIO_PIN_16,
    GPIO_PIN_17,
    GPIO_PIN_20 = 20,
    GPIO_PIN_21,
    GPIO_PIN_22,
    GPIO_PIN_27 = 27,
    GPIO_PIN_28,
    GPIO_PIN_29,
    GPIO_PIN_30,
};

#define GPIO_OUTPUT (1 << 0)
#define GPIO_INPUT (1 << 1)
#define GPIO_FLOAT (0 << 2)
#define GPIO_PULLUP (1 << 3)
#define GPIO_SMT_EN (1 << 4)
#define GPIO_DRV_0 (0 << 5)
#define GPIO_DRV_1 (1 << 5)
#define GPIO_DRV_2 (2 << 5)
#define GPIO_DRV_3 (3 << 5)
#define GPIO_FUNC_SDH (1 << 8)
#define GPIO_ALTERNATE (1 << 9)
#define GPIO_UART_FUNC_UART1_TX 1
#define GPIO_UART_FUNC_UART1_RX 2

void bflb_gpio_init(struct bflb_device_s *dev, int pin, uint32_t config);
void bflb_gpio_deinit(struct bflb_device_s *dev, int pin);
void bflb_gpio_set(struct bflb_device_s *dev, int pin);
void bflb_gpio_reset(struct bflb_device_s *dev, int pin);
int bflb_gpio_read(struct bflb_device_s *dev, int pin);
void bflb_gpio_uart_init(struct bflb_device_s *dev, int pin, int func);

#ifdef __cplusplus
}
#endif
