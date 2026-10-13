// Host sim: shared Bouffalo device handle (opaque on the sim).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bflb_device_s {
    int _sim_id;
    uint32_t reg_base;      // unused on the sim
    uint8_t irq_num;
};

struct bflb_device_s *bflb_device_get_by_name(const char *name);

#ifdef __cplusplus
}
#endif
