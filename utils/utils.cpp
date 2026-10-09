#include "utils.h"

void fpga_tx_header(int cmd, int len) {
    bflb_uart_putchar(uart1_dev, 0xAA);
    bflb_uart_putchar(uart1_dev, len >> 8);
    bflb_uart_putchar(uart1_dev, len & 0xFF);
    bflb_uart_putchar(uart1_dev, cmd);
}

void fpga_tx_byte(uint8_t b) {
    bflb_uart_putchar(uart1_dev, b);
}

uint32_t get_file_size(const char *fname) {
    FILINFO fno;
    FRESULT r = f_stat(fname, &fno);
    if (r != FR_OK) return 0;
    return fno.fsize;
}

// Send a romdata packet to core of len bytes in `fbuf`
void send_fbuf_data(uint16_t len) {
    taskENTER_CRITICAL();
    fpga_tx_header(0x07, len+1);
    for (int i = 0; i < len; i ++) {
        fpga_tx_byte(fbuf[i]);
    }
    taskEXIT_CRITICAL();
    heartbeat_bump();               // ROM transfers call this in a tight loop
}

// set loading state
void set_loading_state(int state) {
    // a ROM load (or reset) mustn't run with the core paused
    if (state && (get_core_config() & CORE_CFG_MENU_PAUSE))
        set_core_config(get_core_config() & ~CORE_CFG_MENU_PAUSE);
    taskENTER_CRITICAL();
    fpga_tx_header(0x06, 2);
    fpga_tx_byte(state);        
    taskEXIT_CRITICAL();
}


// bring FPGA to a good state by sending a few 0's
void send_blank_packet(void) {
    taskENTER_CRITICAL();
    for (int i = 0; i < 8; i++) {
        fpga_tx_byte(0);
    }
    taskEXIT_CRITICAL();
}

#include <string>
#include <algorithm>

const char *cstr_find_ignore_case(const char *str, const char *substr) {
    std::string s(str);
    std::string ss(substr);
    auto it = std::search(s.begin(), s.end(), ss.begin(), ss.end(), [](char a, char b) {
        return std::tolower(a) == std::tolower(b);
    });
    if (it == s.end()) return NULL;
    return str + (it - s.begin());
}

bool has_ext(const char *fname, const char *ext) {
    if (!fname || !ext || !*ext) return false;
    size_t fl = strlen(fname), el = strlen(ext);
    if (fl < el) return false;
    return strcasecmp(fname + fl - el, ext) == 0;
}

bool has_any_ext(const char *fname, const char *exts) {
    if (!fname) return false;
    if (!exts || !*exts) return true;   // no filter: accept everything
    // `exts` is a ';'-separated list like ".bin;.md;.gen;.smd"
    char one[16];
    const char *p = exts;
    while (*p) {
        while (*p == ';') p++;
        if (!*p) break;
        size_t i = 0;
        while (*p && *p != ';') {
            if (i + 1 < sizeof(one)) one[i++] = *p;
            p++;
        }
        one[i] = '\0';
        if (i > 0 && has_ext(fname, one))
            return true;
    }
    return false;
}

static uint32_t core_config;

uint32_t get_core_config(void) {
    return core_config;
}

void forget_core_config(void) {
    core_config = 0;
}

void set_core_config(uint32_t config) {
    core_config = config;
    taskENTER_CRITICAL();
    fpga_tx_header(0x03, 5);
    fpga_tx_byte(config >> 24);
    fpga_tx_byte(config >> 16);
    fpga_tx_byte(config >> 8);
    fpga_tx_byte(config);
    taskEXIT_CRITICAL();
}

/////////////////////////////////////////////////////////////////////////////////
// Shared state among tasks
volatile uint16_t joy1_state = 0;
volatile uint16_t joy2_state = 0;
volatile uint16_t hid1_state = 0;
volatile uint16_t hid2_state = 0;
volatile int16_t core_id = -1;
volatile uint8_t key_buf[4] = {0};
SemaphoreHandle_t state_mutex;              // for all global state access
volatile uint32_t heartbeat;                // last activity tick (watchdog)

// Count a beat, and feed the watchdog here too: some bump sites run inside
// critical sections, where the watchdog task can't.
void heartbeat_bump(void) {
    heartbeat++;
    if (wdg_dev)                    // NULL until main() sets the watchdog up
        bflb_wdg_reset_countervalue(wdg_dev);
}

static uint16_t suppress_mask[4];   // per pad: buttons ignored until released

// read joypad states, without suppressed buttons
void get_joypad_states(uint16_t *joy1, uint16_t *joy2, uint16_t *hid1, uint16_t *hid2)
{
    if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
        uint16_t *pads[4] = {joy1, joy2, hid1, hid2};
        *joy1 = joy1_state;
        *joy2 = joy2_state;
        *hid1 = hid1_state;
        *hid2 = hid2_state;
        for (int i = 0; i < 4; i++) {
            suppress_mask[i] &= *pads[i];       // released: stop ignoring
            *pads[i] &= ~suppress_mask[i];
        }
        xSemaphoreGive(state_mutex);
    }
    heartbeat_bump();       // every menu/input loop calls this: alive
}

void suppress_held_buttons(void)
{
    if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
        suppress_mask[0] |= joy1_state;
        suppress_mask[1] |= joy2_state;
        suppress_mask[2] |= hid1_state;
        suppress_mask[3] |= hid2_state;
        xSemaphoreGive(state_mutex);
    }
}

// query over UART to return if the correct core is loaded
// return >= 0 if request is successful, -1 if timeout (200ms)
int16_t get_core_id(void) {
    if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
        core_id = -1;
        xSemaphoreGive(state_mutex);
    }

    // send command 1
    taskENTER_CRITICAL();
    fpga_tx_header(0x01, 1);
    taskEXIT_CRITICAL();

    // TODO: use a queue for better performance
    uint64_t start = bflb_mtimer_get_time_ms();
    while (bflb_mtimer_get_time_ms() - start < 200) {
        if (xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
            int16_t res = core_id;
            if (res >= 0) {
                xSemaphoreGive(state_mutex);
                return res;
            }
            xSemaphoreGive(state_mutex);
        }
        delay(10);
    }
    return -1;
}