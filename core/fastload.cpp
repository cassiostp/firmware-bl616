// Fast ROM loading: negotiated UART baud + CRC-checked streams (GBA first).
//
// Protocol (the FPGA side is iosys_bl616's FAST_UART): the firmware sends
// 0x15 code at the current rate; a supporting core answers 0x0C (accepted
// code + highest code) at the old rate, and both ends switch once the
// answer has gone out. After the stream, 0x16 asks for the 0x0D load check
// values (CRC-32 and byte count of the rom_do bytes since loading last
// changed), which the firmware compares against the file it sent. A core
// without FAST_UART never answers either command: the load proceeds at 2M
// exactly as before.
#include "fastload.h"

#include "utils.h"
#include "uart_rx.h"
#include "overlay.h"

extern "C" {
#include "bflb_mtimer.h"
}

const uint32_t fast_baud_rates[4] = {2000000, 4000000, 5000000, 8000000};

uint32_t fast_crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return crc;
}

static uint32_t s_baud = 2000000;

uint32_t fast_current_baud(void) {
    return s_baud;
}

bool fast_set_baud(uint32_t baud) {
    if (baud == s_baud)
        return true;
    taskENTER_CRITICAL();
#ifdef TANGCORE_HOST
    sim_uart_set_baud(baud);
#else
    bflb_uart_wait_tx_done(uart1_dev);
    bflb_uart_feature_control(uart1_dev, UART_CMD_SET_BAUD_RATE, (size_t)baud);
    bflb_uart_feature_control(uart1_dev, UART_CMD_CLR_RX_FIFO, 0);
#endif
    uart_rx_ring_reset();       // bytes sampled at the old rate are garbage
    s_baud = baud;
    taskEXIT_CRITICAL();
    return true;
}

// The last 0x0C/0x0D the core sent, stashed by uart1_rx_task (main.cpp).
static int collect_baud_ack(int *accepted) {
    int a = -1;
    if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
        a = fast_baud_ack;
        xSemaphoreGive(state_mutex);
    }
    if (accepted)
        *accepted = a;
    return a;
}

int fast_negotiate(int want_code, int cur_code) {
    if (want_code == cur_code)
        return cur_code;
    fast_reply_reset();
    taskENTER_CRITICAL();
    fpga_tx_header(0x15, 2);
    fpga_tx_byte((uint8_t)want_code);
    taskEXIT_CRITICAL();
    // A core without FAST_UART stays silent: 60 ms, then 2M as before.
    int accepted = -1;
    uint64_t start = bflb_mtimer_get_time_ms();
    while (bflb_mtimer_get_time_ms() - start < 60) {
        if (collect_baud_ack(&accepted) >= 0)
            break;
        delay(2);
    }
    if (accepted != want_code)
        return cur_code;
    fast_set_baud(fast_baud_rates[want_code]);
    delay(2);                       // the answer drains; both ends settle
    if (get_core_id_timeout(100) < 0) {
        // The ends disagree about the rate. Back to the old one: a core
        // that never switched answers here; one that did reverts to 2M
        // after 1 s without a received byte.
        fast_set_baud(fast_baud_rates[cur_code]);
        delay(5);
        if (get_core_id_timeout(100) < 0) {
            delay(1200);
            fast_set_baud(2000000);
            get_core_id_timeout(200);
            return FAST_BAUD_2M;
        }
        return cur_code;
    }
    return want_code;
}

bool fast_verify(uint32_t want_crc, uint32_t want_cnt) {
    fast_reply_reset();
    taskENTER_CRITICAL();
    fpga_tx_header(0x16, 1);
    taskEXIT_CRITICAL();
    uint64_t start = bflb_mtimer_get_time_ms();
    for (;;) {
        bool ready = false;
        uint32_t crc = 0, cnt = 0;
        if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
            ready = fast_load_crc_ready;
            crc = fast_load_crc;
            cnt = fast_load_cnt;
            xSemaphoreGive(state_mutex);
        }
        if (ready) {
            if (crc != want_crc || cnt != want_cnt)
                DEBUG("fast_verify: core crc=%08lx cnt=%lu, want crc=%08lx cnt=%lu\n",
                      (unsigned long)crc, (unsigned long)cnt,
                      (unsigned long)want_crc, (unsigned long)want_cnt);
            return crc == want_crc && cnt == want_cnt;
        }
        if (bflb_mtimer_get_time_ms() - start >= 500) {
            DEBUG("fast_verify: no 0x0D answer\n");
            return false;
        }
        delay(2);
    }
}
