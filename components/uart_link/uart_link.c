#include "uart_link.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_log.h>

#include "data_hub.h"

static const char *TAG = "uart_link";

#define UART_LINK_RX_BUF_SIZE                1024
#define UART_LINK_LINE_MAX                   128
#define UART_LINK_IDN_RESPONSE               "TELEMETRY-HUB,esp32-2432s028r,fw0.1"
#define UART_LINK_BOOT_HANDSHAKE_TIMEOUT_MS  500

static uart_port_t s_uart_num;

// Outstanding-query state. s_query_mutex admits one caller into
// uart_link_query() at a time; s_reply_ready_sem is signaled by the RX
// task once it captures the line following the query.
static SemaphoreHandle_t s_query_mutex;
static SemaphoreHandle_t s_reply_ready_sem;
static volatile bool s_query_pending;
static char *s_reply_buf;
static size_t s_reply_buf_len;
static volatile bool s_mcu_present;

typedef struct {
    const char *query;
    const char *response;
} uart_link_command_t;

// Lines the ESP32 answers itself, without touching data_hub.
static const uart_link_command_t s_local_commands[] = {
    { "*IDN?", UART_LINK_IDN_RESPONSE },
};

static void send_line(const char *text)
{
    uart_write_bytes(s_uart_num, text, strlen(text));
    uart_write_bytes(s_uart_num, "\n", 1);
}

static bool try_answer_locally(const char *line)
{
    for (size_t i = 0; i < sizeof(s_local_commands) / sizeof(s_local_commands[0]); i++) {
        if (strcmp(line, s_local_commands[i].query) == 0) {
            send_line(s_local_commands[i].response);
            return true;
        }
    }
    return false;
}

// Parses a push line shaped like "MEAS:VOLT:DC 12.84,V" — name, a space,
// then value and unit separated by a comma. Unit is optional.
static void handle_push(const char *line)
{
    s_mcu_present = true; // any push line at all is evidence something's out there

    const char *sep = strchr(line, ' ');
    if (sep == NULL || sep == line) {
        ESP_LOGD(TAG, "dropping unparseable push: '%s'", line);
        return;
    }

    char name[DATA_HUB_NAME_LEN];
    size_t name_len = (size_t)(sep - line);
    if (name_len >= sizeof(name)) {
        name_len = sizeof(name) - 1;
    }
    memcpy(name, line, name_len);
    name[name_len] = '\0';

    const char *rest = sep + 1;
    char unit[DATA_HUB_UNIT_LEN] = {0};
    float value = strtof(rest, NULL);

    const char *comma = strchr(rest, ',');
    if (comma != NULL) {
        size_t unit_len = strlen(comma + 1);
        if (unit_len >= sizeof(unit)) {
            unit_len = sizeof(unit) - 1;
        }
        memcpy(unit, comma + 1, unit_len);
        unit[unit_len] = '\0';
    }

    data_hub_publish(name, value, unit);
}

// Handles one complete line (already stripped of \r\n). A query the ESP32
// itself has outstanding always wins the race: if uart_link_query() is
// waiting, the next line in is treated as its reply, even if it happens to
// look like a query or a push. Keeps the dispatch simple; the wire is a
// low-rate text link, so a genuine collision is rare in practice.
static void dispatch_line(char *line)
{
    size_t len = strlen(line);
    if (len == 0) {
        return;
    }

    if (s_query_pending) {
        strncpy(s_reply_buf, line, s_reply_buf_len - 1);
        s_reply_buf[s_reply_buf_len - 1] = '\0';
        s_query_pending = false;
        xSemaphoreGive(s_reply_ready_sem);
        return;
    }

    if (line[len - 1] == '?') {
        if (!try_answer_locally(line)) {
            ESP_LOGD(TAG, "unrecognized query, dropped: '%s'", line);
        }
        return;
    }

    handle_push(line);
}

static void uart_rx_task(void *arg)
{
    char line[UART_LINK_LINE_MAX];
    size_t line_len = 0;
    uint8_t byte;

    while (1) {
        int n = uart_read_bytes(s_uart_num, &byte, 1, portMAX_DELAY);
        if (n <= 0) {
            continue;
        }

        if (byte == '\r') {
            continue;
        }

        if (byte == '\n') {
            line[line_len] = '\0';
            dispatch_line(line);
            line_len = 0;
            continue;
        }

        if (line_len < sizeof(line) - 1) {
            line[line_len++] = (char)byte;
        } else {
            // Line too long — drop it and resync on the next '\n'.
            line_len = 0;
        }
    }
}

esp_err_t uart_link_query(const char *cmd, char *reply_buf, size_t reply_buf_len, uint32_t timeout_ms)
{
    if (cmd == NULL || reply_buf == NULL || reply_buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_query_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_reply_buf = reply_buf;
    s_reply_buf_len = reply_buf_len;
    xSemaphoreTake(s_reply_ready_sem, 0); // drain a stale signal, if any
    s_query_pending = true;

    send_line(cmd);

    esp_err_t err = ESP_OK;
    if (xSemaphoreTake(s_reply_ready_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        s_query_pending = false;
        err = ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(s_query_mutex);
    return err;
}

esp_err_t uart_link_init(const uart_link_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_uart_num = config->uart_num;

    const uart_config_t uart_cfg = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(s_uart_num, UART_LINK_RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(s_uart_num, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(s_uart_num, config->txd_gpio, config->rxd_gpio,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    s_query_mutex = xSemaphoreCreateMutex();
    s_reply_ready_sem = xSemaphoreCreateBinary();
    s_query_pending = false;

    BaseType_t ok = xTaskCreate(uart_rx_task, "uart_rx", 3072, NULL, 10, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create uart_rx task");
        return ESP_FAIL;
    }

    char idn[64];
    esp_err_t err = uart_link_query("*IDN?", idn, sizeof(idn), UART_LINK_BOOT_HANDSHAKE_TIMEOUT_MS);
    if (err == ESP_OK) {
        s_mcu_present = true;
        ESP_LOGI(TAG, "MCU present: %s", idn);
    } else {
        ESP_LOGW(TAG, "no *IDN? reply within %d ms — MCU not detected",
                 UART_LINK_BOOT_HANDSHAKE_TIMEOUT_MS);
    }

    return ESP_OK;
}

bool uart_link_mcu_present(void)
{
    return s_mcu_present;
}
