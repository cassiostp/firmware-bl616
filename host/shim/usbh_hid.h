// Host sim shim for CherryUSB's usbh_hid.h. The USB gamepad stack is
// stubbed out (no devices), so this is only an opaque type.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct usbh_hid {
    int _sim_dummy;
};

#ifdef __cplusplus
}
#endif
