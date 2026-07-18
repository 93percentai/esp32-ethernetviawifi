#include "usb_gadget.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "hid.h"
#include "tinyusb.h"
#include "tusb.h"

static const char *TAG = "usb_gadget";

/*
 * String descriptor indices. These MUST match esp_tinyusb's internal STRID enum
 * (usb_descriptors.c) so that tinyusb_net_init() writes the MAC string into the
 * same slot our NCM descriptor references. With CDC + MSC + NCM all compiled in
 * (CFG_TUD_CDC/MSC/NCM = 1) the enum resolves to:
 *   0 LANGID, 1 MFR, 2 PRODUCT, 3 SERIAL, 4 CDC, 5 MSC, 6 NET, 7 MAC
 * HID adds no STRID entry, so we place the HID interface string last (8).
 */
#define STR_CDC   4
#define STR_MSC   5
#define STR_NET   6
#define STR_MAC   7
#define STR_HID   0   /* esp_tinyusb caps the string table at 8 entries (0..7);
                       * the HID interface uses index 0 = "no string". */
#define STR_COUNT 8

/* Endpoint address assignment. IN endpoints are limited to 4 for functions
 * (numbers 1..4); OUT endpoints reuse the same numbers. No valid combo places
 * ACM together with MSC/HID, so their addresses may overlap across combos. */
#define EP_NCM_NOTIF   0x81
#define EP_NCM_OUT     0x02
#define EP_NCM_IN      0x82
#define EP_ACM_NOTIF   0x83
#define EP_ACM_OUT     0x04
#define EP_ACM_IN      0x84
#define EP_MSC_OUT     0x03
#define EP_MSC_IN      0x83
#define EP_HID_IN_NOMSC 0x83   /* HID data IN when MSC absent */
#define EP_HID_IN_MSC   0x84   /* HID data IN when MSC present */

#define NCM_MAX_SEGMENT 1514

static bool s_active[USB_FUNC_COUNT];

/* ---- HID report descriptor (keyboard + mouse, report IDs) ---- */
static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(HID_REPORT_ID_MOUSE)),
};

const uint8_t *usb_gadget_hid_report_desc(uint16_t *len)
{
    if (len) {
        *len = sizeof(s_hid_report_desc);
    }
    return s_hid_report_desc;
}

/* TinyUSB weak callback: return our combined HID report descriptor. */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

/* ---- Device + string descriptors ---- */
static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* IAD composite device */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,   /* Espressif */
    .idProduct = 0x4020,  /* T-Dongle-S3 multi-mode gadget */
    .bcdDevice = 0x0200,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *s_str_desc[STR_COUNT] = {
    (const char[]){0x09, 0x04},  /* 0: LANGID en-US */
    "LilyGO",                    /* 1: Manufacturer */
    "T-Dongle-S3 Bridge",        /* 2: Product */
    "000000",                    /* 3: Serial (overwritten below) */
    "Console",                   /* 4: CDC-ACM interface */
    "SD Storage",                /* 5: MSC interface */
    "USB Net",                   /* 6: NCM interface */
    "",                          /* 7: MAC (filled by tinyusb_net_init) */
};
static char s_serial[13];

/* ---- Composite configuration descriptors (one per valid combo) ---- */
#define CFG_HDR(itf_count, total) \
    TUD_CONFIG_DESCRIPTOR(1, (itf_count), 0, (total), 0, 100)

#define NCM_DESC \
    TUD_CDC_NCM_DESCRIPTOR(0, STR_NET, STR_MAC, EP_NCM_NOTIF, 64, \
                           EP_NCM_OUT, EP_NCM_IN, 64, NCM_MAX_SEGMENT)

/* A: NCM only */
static const uint8_t s_cfg_ncm[] = {
    CFG_HDR(2, TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN),
    NCM_DESC,
};

/* B: NCM + ACM */
static const uint8_t s_cfg_ncm_acm[] = {
    CFG_HDR(4, TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN + TUD_CDC_DESC_LEN),
    NCM_DESC,
    TUD_CDC_DESCRIPTOR(2, STR_CDC, EP_ACM_NOTIF, 8, EP_ACM_OUT, EP_ACM_IN, 64),
};

/* C: NCM + MSC */
static const uint8_t s_cfg_ncm_msc[] = {
    CFG_HDR(3, TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN + TUD_MSC_DESC_LEN),
    NCM_DESC,
    TUD_MSC_DESCRIPTOR(2, STR_MSC, EP_MSC_OUT, EP_MSC_IN, 64),
};

/* D: NCM + HID */
static const uint8_t s_cfg_ncm_hid[] = {
    CFG_HDR(3, TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN + TUD_HID_DESC_LEN),
    NCM_DESC,
    TUD_HID_DESCRIPTOR(2, STR_HID, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_desc), EP_HID_IN_NOMSC, 16, 10),
};

/* E: NCM + MSC + HID */
static const uint8_t s_cfg_ncm_msc_hid[] = {
    CFG_HDR(4, TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN + TUD_MSC_DESC_LEN + TUD_HID_DESC_LEN),
    NCM_DESC,
    TUD_MSC_DESCRIPTOR(2, STR_MSC, EP_MSC_OUT, EP_MSC_IN, 64),
    TUD_HID_DESCRIPTOR(3, STR_HID, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_desc), EP_HID_IN_MSC, 16, 10),
};

uint8_t usb_func_in_cost(usb_func_t func)
{
    switch (func) {
    case USB_FUNC_ACM: return 2;
    case USB_FUNC_MSC: return 1;
    case USB_FUNC_HID: return 1;
    default: return 0;
    }
}

const char *usb_func_name(usb_func_t func)
{
    switch (func) {
    case USB_FUNC_ACM: return "ACM";
    case USB_FUNC_MSC: return "MSC";
    case USB_FUNC_HID: return "HID";
    default: return "?";
    }
}

static uint8_t optional_in_cost(const bridge_config_t *cfg)
{
    uint8_t sum = 0;
    for (int f = 0; f < USB_FUNC_COUNT; f++) {
        if (config_usb_func_enabled(cfg, (usb_func_t)f)) {
            sum += usb_func_in_cost((usb_func_t)f);
        }
    }
    return sum;
}

static uint8_t evict_until_fits(bridge_config_t *cfg, usb_func_t keep)
{
    uint8_t evicted = 0;
    while (optional_in_cost(cfg) > USB_OPTIONAL_IN_BUDGET) {
        int victim = -1;
        uint32_t best_seq = UINT32_MAX;
        for (int f = 0; f < USB_FUNC_COUNT; f++) {
            if (f == (int)keep) {
                continue;
            }
            if (config_usb_func_enabled(cfg, (usb_func_t)f) &&
                cfg->modes.usb_seq[f] < best_seq) {
                best_seq = cfg->modes.usb_seq[f];
                victim = f;
            }
        }
        if (victim < 0) {
            break;  /* nothing left to evict (should not happen) */
        }
        config_usb_func_set(cfg, (usb_func_t)victim, false);
        evicted |= (uint8_t)(1u << victim);
        ESP_LOGW(TAG, "LRU evicted %s to fit endpoint budget", usb_func_name((usb_func_t)victim));
    }
    return evicted;
}

uint8_t usb_gadget_resolve_enable(bridge_config_t *cfg, usb_func_t func, bool enable)
{
    if (func >= USB_FUNC_COUNT) {
        return 0;
    }
    if (!enable) {
        config_usb_func_set(cfg, func, false);
        return 0;
    }
    config_usb_func_set(cfg, func, true);
    return evict_until_fits(cfg, func);
}

uint8_t usb_gadget_sanitize(bridge_config_t *cfg)
{
    /* keep == COUNT: no protected function, evict purely oldest-first. */
    return evict_until_fits(cfg, USB_FUNC_COUNT);
}

bool usb_gadget_func_active(usb_func_t func)
{
    if (func >= USB_FUNC_COUNT) {
        return false;
    }
    return s_active[func];
}

static const uint8_t *select_descriptor(void)
{
    bool acm = s_active[USB_FUNC_ACM];
    bool msc = s_active[USB_FUNC_MSC];
    bool hid = s_active[USB_FUNC_HID];

    if (msc && hid) {
        return s_cfg_ncm_msc_hid;
    }
    if (msc) {
        return s_cfg_ncm_msc;
    }
    if (hid) {
        return s_cfg_ncm_hid;
    }
    if (acm) {
        return s_cfg_ncm_acm;
    }
    return s_cfg_ncm;
}

esp_err_t usb_gadget_init(const bridge_config_t *cfg)
{
    for (int f = 0; f < USB_FUNC_COUNT; f++) {
        s_active[f] = config_usb_func_enabled(cfg, (usb_func_t)f);
    }

    /* Deterministic serial from the factory eFuse MAC. */
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_str_desc[3] = s_serial;

    const uint8_t *cfg_desc = select_descriptor();

    ESP_LOGI(TAG, "USB composite: NCM + %s%s%s",
             s_active[USB_FUNC_ACM] ? "ACM " : "",
             s_active[USB_FUNC_MSC] ? "MSC " : "",
             s_active[USB_FUNC_HID] ? "HID " : "");

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &s_device_desc,
        .string_descriptor = s_str_desc,
        .string_descriptor_count = STR_COUNT,
        .external_phy = false,
        .configuration_descriptor = cfg_desc,
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
    }
    return err;
}
