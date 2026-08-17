#include "data_hub.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "data_hub";

typedef struct {
    bool in_use;
    char name[DATA_HUB_NAME_LEN];
    char unit[DATA_HUB_UNIT_LEN];
    data_hub_sample_t history[DATA_HUB_HISTORY_LEN];
    size_t head;   // index the next sample will be written to
    size_t count;  // valid samples, saturates at DATA_HUB_HISTORY_LEN
} data_hub_channel_t;

static data_hub_channel_t s_channels[DATA_HUB_MAX_CHANNELS];
static SemaphoreHandle_t s_mutex;

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

void data_hub_init(void)
{
    memset(s_channels, 0, sizeof(s_channels));
    s_mutex = xSemaphoreCreateMutex();
}

static data_hub_channel_t *find_channel_locked(const char *name)
{
    for (size_t i = 0; i < DATA_HUB_MAX_CHANNELS; i++) {
        if (s_channels[i].in_use && strcmp(s_channels[i].name, name) == 0) {
            return &s_channels[i];
        }
    }
    return NULL;
}

static data_hub_channel_t *find_free_channel_locked(void)
{
    for (size_t i = 0; i < DATA_HUB_MAX_CHANNELS; i++) {
        if (!s_channels[i].in_use) {
            return &s_channels[i];
        }
    }
    return NULL;
}

void data_hub_publish(const char *name, float value, const char *unit)
{
    if (name == NULL || name[0] == '\0') {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    data_hub_channel_t *ch = find_channel_locked(name);
    if (ch == NULL) {
        ch = find_free_channel_locked();
        if (ch == NULL) {
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "channel table full, dropping new channel '%s'", name);
            return;
        }
        memset(ch, 0, sizeof(*ch));
        ch->in_use = true;
        copy_str(ch->name, sizeof(ch->name), name);
        ESP_LOGI(TAG, "registered channel '%s'", ch->name);
    }

    copy_str(ch->unit, sizeof(ch->unit), unit);

    data_hub_sample_t *slot = &ch->history[ch->head];
    copy_str(slot->name, sizeof(slot->name), ch->name);
    copy_str(slot->unit, sizeof(slot->unit), ch->unit);
    slot->value = value;
    slot->timestamp_us = esp_timer_get_time();

    ch->head = (ch->head + 1) % DATA_HUB_HISTORY_LEN;
    if (ch->count < DATA_HUB_HISTORY_LEN) {
        ch->count++;
    }

    xSemaphoreGive(s_mutex);
}

bool data_hub_get_latest(const char *name, data_hub_sample_t *out)
{
    bool found = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    data_hub_channel_t *ch = find_channel_locked(name);
    if (ch != NULL && ch->count > 0) {
        size_t last = (ch->head + DATA_HUB_HISTORY_LEN - 1) % DATA_HUB_HISTORY_LEN;
        *out = ch->history[last];
        found = true;
    }

    xSemaphoreGive(s_mutex);
    return found;
}

size_t data_hub_get_history(const char *name, data_hub_sample_t *out, size_t max_out)
{
    size_t copied = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    data_hub_channel_t *ch = find_channel_locked(name);
    if (ch != NULL) {
        size_t n = ch->count < max_out ? ch->count : max_out;
        size_t oldest = (ch->head + DATA_HUB_HISTORY_LEN - ch->count) % DATA_HUB_HISTORY_LEN;
        for (size_t i = 0; i < n; i++) {
            out[i] = ch->history[(oldest + i) % DATA_HUB_HISTORY_LEN];
        }
        copied = n;
    }

    xSemaphoreGive(s_mutex);
    return copied;
}

size_t data_hub_list_channels(data_hub_channel_info_t *out, size_t max_out)
{
    size_t copied = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (size_t i = 0; i < DATA_HUB_MAX_CHANNELS && copied < max_out; i++) {
        if (!s_channels[i].in_use) {
            continue;
        }

        data_hub_channel_t *ch = &s_channels[i];
        data_hub_channel_info_t *info = &out[copied];

        copy_str(info->name, sizeof(info->name), ch->name);
        copy_str(info->unit, sizeof(info->unit), ch->unit);

        if (ch->count > 0) {
            size_t last = (ch->head + DATA_HUB_HISTORY_LEN - 1) % DATA_HUB_HISTORY_LEN;
            info->latest_value = ch->history[last].value;
            info->latest_timestamp_us = ch->history[last].timestamp_us;
        } else {
            info->latest_value = 0.0f;
            info->latest_timestamp_us = 0;
        }

        copied++;
    }

    xSemaphoreGive(s_mutex);
    return copied;
}
