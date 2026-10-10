// Host sim: shared Bouffalo device handle (opaque on the sim).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bflb_device_s {
    int _sim_id;
};

struct bflb_device_s *bflb_device_get_by_name(const char *name);

#ifdef __cplusplus
}
#endif
