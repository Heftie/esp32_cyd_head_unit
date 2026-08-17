#include "sd_storage.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <driver/sdspi_host.h>
#include <sdmmc_cmd.h>
#include <sd_protocol_defs.h>

static const char *TAG = "sd_storage";

#define SD_STORAGE_MOUNT_POINT_LEN        32
#define SD_STORAGE_PATH_LEN               64
#define SD_STORAGE_MAX_OPEN_FILES_DEFAULT 5

static SemaphoreHandle_t s_mutex;
static sdmmc_card_t *s_card;
static bool s_mounted;
static bool s_owns_bus;
static spi_host_device_t s_spi_host;
static char s_mount_point[SD_STORAGE_MOUNT_POINT_LEN];

static esp_err_t build_path(const char *path, char *out, size_t out_size)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int n = snprintf(out, out_size, "%s%s%s", s_mount_point, (path[0] == '/') ? "" : "/", path);
    if (n < 0 || (size_t)n >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t sd_storage_init(const sd_storage_config_t *config)
{
    if (config == NULL || config->mount_point == NULL || config->mount_point[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(config->mount_point) >= sizeof(s_mount_point)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }

    s_owns_bus = !config->bus_already_initialized;
    s_spi_host = config->spi_host;

    if (s_owns_bus) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = config->mosi_gpio,
            .miso_io_num = config->miso_gpio,
            .sclk_io_num = config->clk_gpio,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };
        esp_err_t err = spi_bus_initialize(config->spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    sdmmc_host_t host_cfg = SDSPI_HOST_DEFAULT();
    host_cfg.slot = config->spi_host;
    // Uncontended bus (see components/touch's bit-banged GPIO note) — push
    // to the highest non-UHS SDSPI clock ESP-IDF exposes rather than the
    // macro's default 20 MHz.
    host_cfg.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = config->spi_host;
    slot_cfg.gpio_cs = config->cs_gpio;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = config->format_if_mount_failed,
        .max_files = config->max_open_files ? (int)config->max_open_files : SD_STORAGE_MAX_OPEN_FILES_DEFAULT,
        .allocation_unit_size = 0,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(config->mount_point, &host_cfg, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        if (s_owns_bus) {
            spi_bus_free(config->spi_host);
        }
        return err;
    }

    strncpy(s_mount_point, config->mount_point, sizeof(s_mount_point) - 1);
    s_mount_point[sizeof(s_mount_point) - 1] = '\0';
    s_mounted = true;

    ESP_LOGI(TAG, "mounted at %s (%s, %llu MB)", s_mount_point,
             (s_card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/XC" : "SDSC",
             (unsigned long long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024)));

    return ESP_OK;
}

void sd_storage_deinit(void)
{
    if (!s_mounted) {
        return;
    }

    esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    s_card = NULL;

    if (s_owns_bus) {
        spi_bus_free(s_spi_host);
    }

    s_mounted = false;
}

bool sd_storage_is_mounted(void)
{
    return s_mounted;
}

esp_err_t sd_storage_write(const char *path, const void *data, size_t len, bool append)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    char full[SD_STORAGE_PATH_LEN];
    esp_err_t err = build_path(path, full, sizeof(full));
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    FILE *f = fopen(full, append ? "ab" : "wb");
    if (f == NULL) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "failed to open '%s' for write", full);
        return ESP_FAIL;
    }

    size_t written = (len > 0) ? fwrite(data, 1, len, f) : 0;
    fclose(f);

    xSemaphoreGive(s_mutex);

    return (written == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t sd_storage_read(const char *path, size_t offset, void *buf, size_t buf_len, size_t *out_len)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == NULL || buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char full[SD_STORAGE_PATH_LEN];
    esp_err_t err = build_path(path, full, sizeof(full));
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    FILE *f = fopen(full, "rb");
    if (f == NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (offset > 0) {
        fseek(f, (long)offset, SEEK_SET);
    }
    size_t read_bytes = fread(buf, 1, buf_len, f);
    fclose(f);

    xSemaphoreGive(s_mutex);

    if (out_len != NULL) {
        *out_len = read_bytes;
    }
    return ESP_OK;
}

esp_err_t sd_storage_erase(const char *path)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char full[SD_STORAGE_PATH_LEN];
    esp_err_t err = build_path(path, full, sizeof(full));
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int ret = remove(full);
    xSemaphoreGive(s_mutex);

    if (ret != 0) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sd_storage_format(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = esp_vfs_fat_sdcard_format(s_mount_point, s_card);
    xSemaphoreGive(s_mutex);

    return err;
}

bool sd_storage_exists(const char *path)
{
    size_t size;
    return sd_storage_file_size(path, &size);
}

bool sd_storage_file_size(const char *path, size_t *out_size)
{
    if (!s_mounted || path == NULL) {
        return false;
    }

    char full[SD_STORAGE_PATH_LEN];
    if (build_path(path, full, sizeof(full)) != ESP_OK) {
        return false;
    }

    struct stat st;
    if (stat(full, &st) != 0) {
        return false;
    }

    if (out_size != NULL) {
        *out_size = (size_t)st.st_size;
    }
    return true;
}

esp_err_t sd_storage_get_info(sd_storage_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));

    const char *type = s_card->is_mmc ? "MMC" : (s_card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/XC" : "SDSC";
    strncpy(out->card_type, type, sizeof(out->card_type) - 1);

    out->card_capacity_bytes = (uint64_t)s_card->csd.capacity * (uint64_t)s_card->csd.sector_size;

    return esp_vfs_fat_info(s_mount_point, &out->total_bytes, &out->free_bytes);
}
