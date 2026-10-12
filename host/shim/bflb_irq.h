// Host sim shim for Bouffalo bflb_irq.h.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*irq_callback)(int irq, void *arg);

void bflb_irq_attach(int irq, irq_callback isr, void *arg);
void bflb_irq_enable(int irq);
void bflb_irq_disable(int irq);

#ifdef __cplusplus
}
#endif
