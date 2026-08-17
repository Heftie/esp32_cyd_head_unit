#include "logger.h"

#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>

#include "data_hub.h"
#include "sd_storage.h"

static const char *TAG = "logger";

#define LOGGER_LOG_PATH_LEN       64
#define LOGGER_DEFAULT_INTERVAL_MS 5000
#define LOGGER_CHUNK_BYTES        8192 // holds a full DATA_HUB_HISTORY_LEN catch-up in one write

typedef struct {
    char name[DATA_HUB_NAME_LEN];
    int64_t last_logged_ts;
} logger_channel_state_t;

static logger_channel_state_t s_state[DATA_HUB_MAX_CHANNELS];
static size_t s_state_count;
static char s_log_path[LOGGER_LOG_PATH_LEN];
static uint32_t s_flush_interval_ms;

// Channels are only ever added, matching data_hub's own append-only table —
// so a plain linear scan over this small array is enough to track "have we
// logged this sample already" per channel.
static int64_t *find_or_add_state(const char *name)
{
    for (size_t i = 0; i < s_state_count; i++) {
        if (strcmp(s_state[i].name, name) == 0) {
            return &s_state[i].last_logged_ts;
        }
    }
    if (s_state_count >= DATA_HUB_MAX_CHANNELS) {
        return NULL;
    }
    logger_channel_state_t *slot = &s_state[s_state_count++];
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';
    slot->last_logged_ts = 0;
    return &slot->last_logged_ts;
}

// flush_channel only ever runs serially from logger_task, so these buffers
// (~13 KB combined) live in .bss rather than on the task's stack.
static data_hub_sample_t s_history[DATA_HUB_HISTORY_LEN];
static char s_buf[LOGGER_CHUNK_BYTES];

static size_t flush_channel(const char *name)
{
    int64_t *last_ts = find_or_add_state(name);
    if (last_ts == NULL) {
        ESP_LOGW(TAG, "channel table full, dropping '%s'", name);
        return 0;
    }

    size_t n = data_hub_get_history(name, s_history, DATA_HUB_HISTORY_LEN);

    size_t buf_len = 0;
    size_t written = 0;
    int64_t new_last_ts = *last_ts;

    for (size_t i = 0; i < n; i++) {
        if (s_history[i].timestamp_us <= *last_ts) {
            continue;
        }
        int line_len = snprintf(s_buf + buf_len, sizeof(s_buf) - buf_len, "%lld,%s,%.4f,%s\n",
                                 (long long)s_history[i].timestamp_us, s_history[i].name,
                                 s_history[i].value, s_history[i].unit);
        if (line_len < 0 || (size_t)line_len >= sizeof(s_buf) - buf_len) {
            break; // out of room; whatever's left is picked up next flush
        }
        buf_len += (size_t)line_len;
        new_last_ts = s_history[i].timestamp_us;
        written++;
    }

    if (written == 0) {
        return 0;
    }

    esp_err_t err = sd_storage_write(s_log_path, s_buf, buf_len, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flush of '%s' failed: %s", name, esp_err_to_name(err));
        return 0;
    }
    *last_ts = new_last_ts;
    return written;
}

static void logger_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(s_flush_interval_ms));

        data_hub_channel_info_t channels[DATA_HUB_MAX_CHANNELS];
        size_t n = data_hub_list_channels(channels, DATA_HUB_MAX_CHANNELS);
        size_t total_written = 0;
        for (size_t i = 0; i < n; i++) {
            total_written += flush_channel(channels[i].name);
        }
        if (total_written > 0) {
            ESP_LOGI(TAG, "flushed %u row(s) across %u channel(s) to '%s'",
                     (unsigned)total_written, (unsigned)n, s_log_path);
        }
    }
}

esp_err_t logger_init(const logger_config_t *config)
{
    if (config == NULL || config->log_path == NULL || config->log_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sd_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(config->log_path) >= sizeof(s_log_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(s_log_path, config->log_path, sizeof(s_log_path) - 1);
    s_log_path[sizeof(s_log_path) - 1] = '\0';
    s_flush_interval_ms = config->flush_interval_ms ? config->flush_interval_ms : LOGGER_DEFAULT_INTERVAL_MS;
    s_state_count = 0;

    if (!sd_storage_exists(s_log_path)) {
        static const char header[] = "timestamp_us,channel,value,unit\n";
        esp_err_t err = sd_storage_write(s_log_path, header, sizeof(header) - 1, false);
        if (err != ESP_OK) {
            return err;
        }
    }

    BaseType_t ok = xTaskCreate(logger_task, "logger", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create logger task");
        return ESP_FAIL;
    }

    return ESP_OK;
}
