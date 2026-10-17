#pragma once
#include <stdint.h>
#include <stddef.h>

// Fast ROM loading: negotiated UART baud + CRC-checked streams (GBA first).
// Baud codes on the wire (FPGA iosys FAST_UART, commands 0x15/0x0C):
// 0 = 2 Mbaud (the power-on rate), 1 = 4M, 2 = 5M, 3 = 8M.
enum { FAST_BAUD_2M = 0, FAST_BAUD_4M = 1, FAST_BAUD_5M = 2, FAST_BAUD_8M = 3 };
extern const uint32_t fast_baud_rates[4];

// zlib CRC-32 over p[0..n): init crc to 0xFFFFFFFF, xor the result with
// 0xFFFFFFFF to compare against the core's 0x0D answer.
uint32_t fast_crc32_update(uint32_t crc, const uint8_t *p, size_t n);

// MCU UART baud right now (2M unless a fast load negotiated up).
uint32_t fast_current_baud(void);
// Switch the MCU UART to baud (drains TX first, drops stale RX).
bool fast_set_baud(uint32_t baud);
// Back to the power-on rate (fresh bitstreams always answer there).
static inline bool fast_set_baud_2m(void) { return fast_set_baud(2000000); }

// Ask the core to move the link to want_code (sent at the current baud):
// up to 60 ms for its 0x0C answer, then switch the MCU when accepted and
// confirm with a core-ID poll at the new rate. Returns the code both ends
// now use: want_code, or cur_code when the core stays silent, refuses, or
// the confirm fails (want_code == cur_code is a no-op). A core without
// FAST_UART never answers, so it loads at 2M exactly as before.
int fast_negotiate(int want_code, int cur_code);

// Ask the core for its load check values (0x16 -> 0x0D: CRC-32 and byte
// count of the rom_do stream since loading last changed) and compare with
// the caller's. False on timeout or mismatch.
bool fast_verify(uint32_t want_crc, uint32_t want_cnt);

#ifdef TANGCORE_HOST
// Host sim only: tell the fake/RTL FPGA backend the MCU switched baud (see
// host/sim/stubs.cpp). The real switch is bflb_uart_feature_control above.
void sim_uart_set_baud(uint32_t baud);
#endif
