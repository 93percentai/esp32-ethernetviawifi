#include "sdcard.h"

#include <string.h>

#include "board.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_protocol_defs.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";
static const char *BASE_PATH = "/sd";

#ifndef CONFIG_BRIDGE_SD_BUS_WIDTH
#define CONFIG_BRIDGE_SD_BUS_WIDTH 1
#endif

#define SD_ACTIVE_WINDOW_MS 1500

static sdmmc_host_t s_host;
static sdmmc_card_t *s_card;
static bool s_present;
static esp_err_t s_init_err = ESP_ERR_NOT_FOUND;
static volatile sd_owner_t s_owner = SD_OWNER_NONE;

static SemaphoreHandle_t s_stats_lock;
static struct {
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint32_t read_ops;
    uint32_t write_ops;
    int64_t last_read_us;
    int64_t last_write_us;
} s_io;

static void stats_lock(void)
{
    if (s_stats_lock) {
        xSemaphoreTake(s_stats_lock, portMAX_DELAY);
    }
}

static void stats_unlock(void)
{
    if (s_stats_lock) {
        xSemaphoreGive(s_stats_lock);
    }
}

esp_err_t sdcard_init(void)
{
    if (s_present) {
        return ESP_OK;
    }
    s_stats_lock = xSemaphoreCreateMutex();
    s_io.last_read_us = -1;
    s_io.last_write_us = -1;

    s_host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    s_host.slot = SDMMC_HOST_SLOT_1;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = CONFIG_BRIDGE_SD_BUS_WIDTH;
    slot.clk = BOARD_SD_PIN_CLK;
    slot.cmd = BOARD_SD_PIN_CMD;
    slot.d0 = BOARD_SD_PIN_D0;
    slot.d1 = BOARD_SD_PIN_D1;
    slot.d2 = BOARD_SD_PIN_D2;
    slot.d3 = BOARD_SD_PIN_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sdmmc_host_init failed: %s", esp_err_to_name(err));
        s_init_err = err;
        return err;
    }
    err = sdmmc_host_init_slot(s_host.slot, &slot);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sdmmc_host_init_slot failed: %s", esp_err_to_name(err));
        s_init_err = err;
        sdmmc_host_deinit();
        return err;
    }

    s_card = calloc(1, sizeof(sdmmc_card_t));
    if (!s_card) {
        s_init_err = ESP_ERR_NO_MEM;
        sdmmc_host_deinit();
        return ESP_ERR_NO_MEM;
    }

    err = sdmmc_card_init(&s_host, s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No SD card / init failed (%s), width=%d",
                 esp_err_to_name(err), CONFIG_BRIDGE_SD_BUS_WIDTH);
        s_init_err = err;
        free(s_card);
        s_card = NULL;
        sdmmc_host_deinit();
        return ESP_ERR_NOT_FOUND;
    }

    s_init_err = ESP_OK;
    s_present = true;
    ESP_LOGI(TAG, "SD card: %llu MB, %u-byte sectors, width=%d",
             sdcard_capacity_bytes() / (1024ULL * 1024ULL),
             (unsigned)sdcard_sector_size(), CONFIG_BRIDGE_SD_BUS_WIDTH);
    return ESP_OK;
}

bool sdcard_present(void)
{
    return s_present;
}

uint64_t sdcard_capacity_bytes(void)
{
    if (!s_present) {
        return 0;
    }
    return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
}

const char *sdcard_last_error(void)
{
    return esp_err_to_name(s_init_err);
}

const char *sdcard_type_str(void)
{
    if (!s_present) {
        return "none";
    }
    if (s_card->is_mmc) {
        return "MMC";
    }
    return (s_card->ocr & SD_OCR_SDHC_CAP) ? "SDHC" : "SDSC";
}

uint32_t sdcard_sector_count(void)
{
    return s_present ? s_card->csd.capacity : 0;
}

uint32_t sdcard_sector_size(void)
{
    return s_present ? s_card->csd.sector_size : 0;
}

esp_err_t sdcard_read_sectors(uint32_t start_sector, uint32_t count, void *dst)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = sdmmc_read_sectors(s_card, dst, start_sector, count);
    if (err == ESP_OK) {
        stats_lock();
        s_io.read_bytes += (uint64_t)count * s_card->csd.sector_size;
        s_io.read_ops++;
        s_io.last_read_us = esp_timer_get_time();
        stats_unlock();
    }
    return err;
}

esp_err_t sdcard_write_sectors(uint32_t start_sector, uint32_t count, const void *src)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = sdmmc_write_sectors(s_card, src, start_sector, count);
    if (err == ESP_OK) {
        stats_lock();
        s_io.write_bytes += (uint64_t)count * s_card->csd.sector_size;
        s_io.write_ops++;
        s_io.last_write_us = esp_timer_get_time();
        stats_unlock();
    }
    return err;
}

sd_owner_t sdcard_owner(void)
{
    return s_owner;
}

const char *sdcard_base_path(void)
{
    return BASE_PATH;
}

bool sdcard_fs_mounted(void)
{
    return s_owner == SD_OWNER_ESP;
}

esp_err_t sdcard_take_esp(void)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_owner == SD_OWNER_ESP) {
        return ESP_OK;
    }

    BYTE pdrv = 0xFF;
    esp_err_t err = ff_diskio_get_drive(&pdrv);
    if (err != ESP_OK || pdrv == 0xFF) {
        ESP_LOGE(TAG, "No free FATFS drive");
        return ESP_FAIL;
    }
    char drv[3] = { (char)('0' + pdrv), ':', 0 };

    ff_diskio_register_sdmmc(pdrv, s_card);
    ff_sdmmc_set_disk_status_check(pdrv, false);

    FATFS *fs = NULL;
    const esp_vfs_fat_conf_t conf = {
        .base_path = BASE_PATH,
        .fat_drive = drv,
        .max_files = 4,
    };
    err = esp_vfs_fat_register(&conf, &fs);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_vfs_fat_register failed: %s", esp_err_to_name(err));
        ff_diskio_unregister(pdrv);
        return err;
    }

    FRESULT fr = f_mount(fs, drv, 1);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "f_mount failed (%d) — card may be unformatted", fr);
        esp_vfs_fat_unregister_path(BASE_PATH);
        ff_diskio_unregister(pdrv);
        return ESP_FAIL;
    }

    s_owner = SD_OWNER_ESP;
    ESP_LOGI(TAG, "SD FAT mounted at %s (owner=ESP)", BASE_PATH);
    return ESP_OK;
}

esp_err_t sdcard_release_to_host(void)
{
    if (s_owner == SD_OWNER_ESP) {
        esp_vfs_fat_unregister_path(BASE_PATH);
        BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
        if (pdrv != 0xFF) {
            char drv[3] = { (char)('0' + pdrv), ':', 0 };
            f_mount(0, drv, 0);
            ff_diskio_unregister(pdrv);
        }
        ESP_LOGI(TAG, "SD FAT unmounted (owner=HOST)");
    }
    s_owner = s_present ? SD_OWNER_HOST : SD_OWNER_NONE;
    return ESP_OK;
}

void sdcard_get_io_stats(sd_io_stats_t *out)
{
    if (!out) {
        return;
    }
    stats_lock();
    int64_t now = esp_timer_get_time();
    out->read_bytes = s_io.read_bytes;
    out->write_bytes = s_io.write_bytes;
    out->read_ops = s_io.read_ops;
    out->write_ops = s_io.write_ops;
    out->ms_since_read = (s_io.last_read_us < 0) ? UINT32_MAX
                         : (uint32_t)((now - s_io.last_read_us) / 1000);
    out->ms_since_write = (s_io.last_write_us < 0) ? UINT32_MAX
                          : (uint32_t)((now - s_io.last_write_us) / 1000);
    out->active = (out->ms_since_read < SD_ACTIVE_WINDOW_MS) ||
                  (out->ms_since_write < SD_ACTIVE_WINDOW_MS);
    stats_unlock();
}
