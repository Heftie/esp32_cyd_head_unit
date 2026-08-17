#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uart_port_t uart_num;
    int txd_gpio;
    int rxd_gpio;
    int baud_rate;
} uart_link_config_t;

// Brings up the UART, starts the RX/dispatch task, and performs a one-shot
// *IDN? handshake with the companion MCU (logged, not fatal on timeout).
esp_err_t uart_link_init(const uart_link_config_t *config);

// Sends `cmd` (a trailing '\n' is appended for you) and blocks up to
// timeout_ms for the MCU's reply line. Only one query may be outstanding
// at a time; a concurrent caller waits up to timeout_ms for its turn, then
// up to timeout_ms again for its own reply.
esp_err_t uart_link_query(const char *cmd, char *reply_buf, size_t reply_buf_len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
