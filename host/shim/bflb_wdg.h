// Host sim shim for Bouffalo bflb_wdg.h. Feeds are counted, never reset.
#pragma once

#include "bflb_device.h"
#include "bflb_mtimer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WDG_CLKSRC_32K 0
#define WDG_MODE_RESET 1

struct bflb_wdg_config_s {
    int clock_source;
    int clock_div;
    int comp_val;
    int mode;
};

void bflb_wdg_init(struct bflb_device_s *dev, const struct bflb_wdg_config_s *cfg);
void bflb_wdg_start(struct bflb_device_s *dev);
void bflb_wdg_stop(struct bflb_device_s *dev);
void bflb_wdg_reset_countervalue(struct bflb_device_s *dev);

// Sim-only introspection (defined by the sim, C++ linkage via sim headers).
#ifdef __cplusplus
}
#endif
