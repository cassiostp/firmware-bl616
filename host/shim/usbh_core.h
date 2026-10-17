// Host sim shim for CherryUSB's usbh_core.h. USB devices are stubbed:
// no gamepads or MSC drives are ever enumerated.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct usbh_msc {
    int _sim_dummy;
};

void usbh_initialize(void);

#ifdef __cplusplus
}
#endif
