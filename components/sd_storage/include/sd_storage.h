#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <esp_err.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t spi_host;  // SPI peripheral the card's CS line sits on
    gpio_num_t cs_gpio;          // card's dedicated CS line

    // If the SPI bus (CLK/MOSI/MISO) for `spi_host` has already been brought
    // up by another component sharing the same bus, set this true and leave
    // clk/mosi/miso_gpio unset — sd_storage then only attaches its own CS
    // device instead of calling spi_bus_initialize() itself.
    bool bus_already_initialized;
    gpio_num_t clk_gpio;
    gpio_num_t mosi_gpio;
    gpio_num_t miso_gpio;

    const char *mount_point;            // e.g. "/sdcard"
    size_t      max_open_files;         // 0 = driver default (5)
    bool        format_if_mount_failed; // reformat instead of failing init on an unreadable/blank card
} sd_storage_config_t;

typedef struct {
    char     card_type[8];        // "SDSC", "SDHC/XC", or "MMC"
    uint64_t card_capacity_bytes; // raw card capacity
    uint64_t total_bytes;         // filesystem total space
    uint64_t free_bytes;          // filesystem free space
} sd_storage_info_t;

#define SD_STORAGE_NAME_LEN 64

typedef struct {
    char name[SD_STORAGE_NAME_LEN]; // filename only, no path
    uint64_t size_bytes;
    time_t mtime; // last-modified time, from FATFS's get_fattime(); see
                  // sd_storage_list_dir()'s comment for what this
                  // actually needs to be meaningful
} sd_storage_dir_entry_t;

// Mounts the card and brings up a FAT filesystem at config->mount_point.
// Safe to call again after sd_storage_deinit().
esp_err_t sd_storage_init(const sd_storage_config_t *config);

// Unmounts the card. Also releases the SPI bus, if sd_storage owns it.
void sd_storage_deinit(void);

// True once init() has succeeded and the card is currently mounted.
bool sd_storage_is_mounted(void);

// Writes `len` bytes from `data` to `path` (relative to the mount point,
// e.g. "log.csv" or "/log.csv"). Overwrites any existing file unless
// `append` is true. Creates the file if it doesn't exist yet.
esp_err_t sd_storage_write(const char *path, const void *data, size_t len, bool append);

// Reads up to `buf_len` bytes from `path` into `buf`, starting `offset`
// bytes into the file. `out_len` (optional) receives the number of bytes
// actually read — less than buf_len at end-of-file.
esp_err_t sd_storage_read(const char *path, size_t offset, void *buf, size_t buf_len, size_t *out_len);

// Deletes a single file.
esp_err_t sd_storage_erase(const char *path);

// Renames/moves a file (both paths relative to the mount point). If
// new_path already exists it's overwritten first — FATFS's rename() isn't
// atomic-replace, so a stale target would otherwise make this fail with
// nothing renamed. Intended for logger.c's single-generation log rotation
// (current file -> ".1" backup, overwriting any older backup).
esp_err_t sd_storage_rename(const char *old_path, const char *new_path);

// Wipes the whole card and re-creates a fresh, empty FAT filesystem.
// Irreversible.
esp_err_t sd_storage_format(void);

// True if `path` exists on the card.
bool sd_storage_exists(const char *path);

// Fills *out_size with the size of `path` in bytes. Returns false if the
// file doesn't exist.
bool sd_storage_file_size(const char *path, size_t *out_size);

// Card type/capacity plus filesystem total/free space.
esp_err_t sd_storage_get_info(sd_storage_info_t *out);

// Lists regular files directly under `path` (NULL or "" for the mount
// point's root) into `out`, up to `max_out` entries. Skips subdirectories
// — this card is used flat, one file per log, so there's nothing else
// worth descending into. Returns the number of entries copied; 0 if
// nothing is mounted, `path` doesn't exist, or it's empty.
size_t sd_storage_list_dir(const char *path, sd_storage_dir_entry_t *out, size_t max_out);

#ifdef __cplusplus
}
#endif
