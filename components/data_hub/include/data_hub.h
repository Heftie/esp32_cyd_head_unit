#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DATA_HUB_MAX_CHANNELS 16
#define DATA_HUB_NAME_LEN     16
#define DATA_HUB_UNIT_LEN     8
#define DATA_HUB_HISTORY_LEN  128

typedef struct {
    char name[DATA_HUB_NAME_LEN];
    char unit[DATA_HUB_UNIT_LEN];
    float value;
    int64_t timestamp_us;
} data_hub_sample_t;

typedef struct {
    char name[DATA_HUB_NAME_LEN];
    char unit[DATA_HUB_UNIT_LEN];
    float latest_value;
    int64_t latest_timestamp_us;
} data_hub_channel_info_t;

void data_hub_init(void);

// Registers the channel on first sight. Called only by uart_link.
void data_hub_publish(const char *name, float value, const char *unit);

// Fills *out with the newest sample for `name`. Returns false if the
// channel doesn't exist yet.
bool data_hub_get_latest(const char *name, data_hub_sample_t *out);

// Copies up to max_out samples for `name`, oldest first. Returns the
// number of samples copied.
size_t data_hub_get_history(const char *name, data_hub_sample_t *out, size_t max_out);

// Copies up to max_out registered channels' latest state into out.
// Returns the number of channels copied.
size_t data_hub_list_channels(data_hub_channel_info_t *out, size_t max_out);

#ifdef __cplusplus
}
#endif
