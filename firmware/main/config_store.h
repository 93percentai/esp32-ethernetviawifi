#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CFG_SSID_MAX     33
#define CFG_PASS_MAX     64
#define CFG_PROFILE_MAX  8
#define CFG_ACTIVE_NONE  0xFF

typedef struct {
    char ssid[CFG_SSID_MAX];
    char password[CFG_PASS_MAX];
} wifi_profile_t;

/*
 * Optional USB functions that compete for the ESP32-S3's limited USB IN
 * endpoints (see usb_gadget.h). CDC-NCM is a pinned base function and is not
 * listed here. Order is stable (persisted in NVS) — do not reorder.
 */
typedef enum {
    USB_FUNC_ACM = 0,   /* CDC-ACM management console */
    USB_FUNC_MSC,       /* SD card exposed as USB mass storage */
    USB_FUNC_HID,       /* USB HID keyboard + mouse */
    USB_FUNC_COUNT
} usb_func_t;

/*
 * Runtime feature flags. USB functions (acm/msc/hid) carry a monotonic
 * enable sequence used by the LRU endpoint-eviction policy in usb_gadget.
 * "share" (WebDAV + web UI) is a network feature, not a USB function, so it
 * has no endpoint cost; it (and hid) request the NAT tether network mode.
 */
typedef struct {
    uint8_t  usb_enabled[USB_FUNC_COUNT];  /* bool per USB function */
    uint32_t usb_seq[USB_FUNC_COUNT];      /* enable order; 0 = never enabled */
    uint32_t seq_next;                     /* next sequence value to assign */
    uint8_t  share_enabled;                /* WebDAV + web UI over the LAN */
} modes_config_t;

typedef struct {
    /* --- legacy v1 layout: keep first and do not reorder (NVS migration) --- */
    uint8_t profile_count;
    uint8_t active;          /* index or CFG_ACTIVE_NONE */
    uint8_t debug_enabled;
    wifi_profile_t profiles[CFG_PROFILE_MAX];
    /* --- v2 additions --- */
    modes_config_t modes;
} bridge_config_t;

void config_defaults(bridge_config_t *cfg);
void config_load(bridge_config_t *cfg);
bool config_save(const bridge_config_t *cfg);

/* --- USB function / mode helpers --- */

bool config_usb_func_enabled(const bridge_config_t *cfg, usb_func_t func);
/* Enable/disable a USB function, maintaining the LRU enable sequence.
 * Does not apply endpoint eviction — that is usb_gadget's job. */
void config_usb_func_set(bridge_config_t *cfg, usb_func_t func, bool enable);

bool config_share_enabled(const bridge_config_t *cfg);
void config_share_set(bridge_config_t *cfg, bool enable);

/* True when the ESP should hold its own IP and NAT the USB host, i.e. when
 * the network share or the network-controlled HID feature is enabled. */
bool config_nat_wanted(const bridge_config_t *cfg);

/* Wipe all runtime modes back to defaults (ACM console on, everything off). */
void config_clear_modes(bridge_config_t *cfg);

const char *config_active_ssid(const bridge_config_t *cfg);
const char *config_active_pass(const bridge_config_t *cfg);

int config_find_profile(const bridge_config_t *cfg, const char *ssid);
int config_add_profile(bridge_config_t *cfg, const char *ssid);
void config_del_profile(bridge_config_t *cfg, int index);

/* Wipe all Wi-Fi profiles in RAM (does not touch NVS until config_save). */
void config_clear_wifi(bridge_config_t *cfg);
