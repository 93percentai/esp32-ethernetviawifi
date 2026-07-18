#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * microSD (SDMMC) support for the T-Dongle-S3 hidden TF slot.
 *
 * The card is initialised once and its raw sector interface is shared between
 * two consumers that must never touch the FAT filesystem at the same time:
 *   - USB Mass Storage (host owns the raw block device), and
 *   - the on-device FAT mount used by the WebDAV / web file share.
 *
 * sdcard owns a simple single-owner arbiter plus per-transfer I/O counters so
 * the UI can show when the host is actively reading/writing.
 */

typedef enum {
    SD_OWNER_NONE = 0,  /* card idle: neither exposed to host nor FAT-mounted */
    SD_OWNER_HOST,      /* exposed to USB host as mass storage */
    SD_OWNER_ESP,       /* FAT mounted on-device for the web/WebDAV share */
} sd_owner_t;

typedef struct {
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint32_t read_ops;
    uint32_t write_ops;
    uint32_t ms_since_read;   /* UINT32_MAX if never */
    uint32_t ms_since_write;  /* UINT32_MAX if never */
    bool active;              /* a read or write happened within the last ~1.5 s */
} sd_io_stats_t;

/* Probe and initialise the SD card. Returns ESP_ERR_NOT_FOUND if no card. */
esp_err_t sdcard_init(void);

bool sdcard_present(void);
uint64_t sdcard_capacity_bytes(void);
const char *sdcard_type_str(void);

/* Raw sector access (used by the USB MSC callbacks). Updates I/O counters. */
esp_err_t sdcard_read_sectors(uint32_t start_sector, uint32_t count, void *dst);
esp_err_t sdcard_write_sectors(uint32_t start_sector, uint32_t count, const void *src);
uint32_t sdcard_sector_count(void);
uint32_t sdcard_sector_size(void);

/* Single-owner arbiter. Switching to ESP mounts FATFS; switching to HOST (or
 * NONE) unmounts it so the host can safely own the raw block device. */
sd_owner_t sdcard_owner(void);
esp_err_t sdcard_take_esp(void);    /* mount FAT for the on-device share */
esp_err_t sdcard_release_to_host(void); /* unmount FAT, expose to USB host */
bool sdcard_fs_mounted(void);
const char *sdcard_base_path(void);

void sdcard_get_io_stats(sd_io_stats_t *out);
