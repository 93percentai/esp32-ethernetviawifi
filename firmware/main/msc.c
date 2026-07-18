/*
 * USB Mass Storage (MSC) class callbacks.
 *
 * Unlike esp_tinyusb's built-in tusb_msc_storage helper, this firmware provides
 * its own tud_msc_* callbacks so that host block transfers flow through
 * sdcard_read_sectors()/sdcard_write_sectors(), which maintain per-transfer
 * read/write activity counters. The web UI uses those counters to warn before a
 * "force unmount" would interrupt an in-flight host transfer.
 *
 * The SD card is only presented to the host while sdcard_owner() == SD_OWNER_HOST.
 * When the on-device FAT share owns the card (SD_OWNER_ESP), TEST UNIT READY
 * reports "medium not present" so the host cannot read/write stale blocks.
 *
 * After ownership changes we raise UNIT ATTENTION so the host re-reads capacity
 * and directory contents instead of keeping a stale cached size/view.
 */
#include <string.h>

#include "class/msc/msc.h"
#include "esp_log.h"
#include "msc.h"
#include "sdcard.h"
#include "tusb.h"

static const char *TAG = "msc";

/* Set when the logical unit appears/disappears so the next TUR fails with UA. */
static volatile bool s_unit_attention;

static void pad_str(uint8_t *dst, size_t dst_len, const char *src)
{
    size_t n = strlen(src);
    if (n > dst_len) {
        n = dst_len;
    }
    memcpy(dst, src, n);
    if (n < dst_len) {
        memset(dst + n, ' ', dst_len - n);
    }
}

static bool medium_ready(void)
{
    return sdcard_present() && sdcard_owner() == SD_OWNER_HOST && sdcard_sector_count() > 0;
}

void msc_register(void)
{
    ESP_LOGI(TAG, "MSC callbacks linked");
}

void msc_notify_media_changed(void)
{
    s_unit_attention = true;
    ESP_LOGI(TAG, "MSC media change — host will re-enumerate capacity");
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4])
{
    (void)lun;
    pad_str(vendor_id, 8, "LilyGO");
    pad_str(product_id, 16, "T-Dongle SD");
    pad_str(product_rev, 4, "1.0");
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!medium_ready()) {
        /* Not ready: either no card or the on-device share currently owns it. */
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00); /* medium not present */
        return false;
    }
    if (s_unit_attention) {
        /* ASC 0x28: NOT READY TO READY transition / medium may have changed. */
        s_unit_attention = false;
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    /* Always advertise 512-byte logical blocks — SDMMC uses 512-byte sectors. */
    *block_size = 512;
    if (!medium_ready()) {
        /* Force the host to re-query after the medium becomes ready again. */
        *block_count = 0;
        return;
    }
    *block_count = sdcard_sector_count();
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return medium_ready();
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    if (load_eject && !start) {
        /* Host ejected the volume — keep owner as HOST but mark attention for reinsert. */
        s_unit_attention = true;
        ESP_LOGI(TAG, "Host ejected MSC medium");
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer,
                          uint32_t bufsize)
{
    (void)lun;
    if (!medium_ready()) {
        return -1;
    }
    const uint32_t ss = 512;
    if ((offset % ss) != 0 || (bufsize % ss) != 0) {
        return -1;
    }
    uint32_t sector = lba + offset / ss;
    uint32_t count = bufsize / ss;
    if (sector + count > sdcard_sector_count()) {
        return -1;
    }
    if (sdcard_read_sectors(sector, count, buffer) != ESP_OK) {
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                           uint32_t bufsize)
{
    (void)lun;
    if (!medium_ready()) {
        return -1;
    }
    const uint32_t ss = 512;
    if ((offset % ss) != 0 || (bufsize % ss) != 0) {
        return -1;
    }
    uint32_t sector = lba + offset / ss;
    uint32_t count = bufsize / ss;
    if (sector + count > sdcard_sector_count()) {
        return -1;
    }
    if (sdcard_write_sectors(sector, count, buffer) != ESP_OK) {
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;
    default:
        ESP_LOGD(TAG, "unsupported SCSI cmd 0x%02x", scsi_cmd[0]);
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}
