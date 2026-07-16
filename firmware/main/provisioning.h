#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config_store.h"
#include "esp_err.h"

#define PROV_SOFTAP_SSID "ESP_WIFITOUSB_CONF"

/* SoftAP captive-portal IP (user-facing config URL). */
#define PROV_SOFTAP_IP_A 192
#define PROV_SOFTAP_IP_B 168
#define PROV_SOFTAP_IP_C 1
#define PROV_SOFTAP_IP_D 1

typedef enum {
    PROV_PHASE_IDLE = 0,
    PROV_PHASE_SCANNING,
    PROV_PHASE_PORTAL,
    PROV_PHASE_TESTING,
    PROV_PHASE_SUCCESS,
} prov_phase_t;

typedef struct {
    bool active;
    prov_phase_t phase;
    int scan_count;
    int clients;
    char detail[28];
} provisioning_lcd_status_t;

/**
 * When no SSID is configured: scan nearby networks, start SoftAP
 * ESP_WIFITOUSB_CONF at 192.168.1.1 with a captive-portal config page.
 * On a successful Wi-Fi test, credentials are saved and SoftAP stops.
 */
esp_err_t provisioning_start(bridge_config_t *cfg);

/** Tear down SoftAP / HTTP / DNS if active. Does not associate STA. */
void provisioning_stop(void);

bool provisioning_is_active(void);

/** Live SoftAP/portal status for the LCD HUD (safe to call anytime). */
void provisioning_get_lcd_status(provisioning_lcd_status_t *out);

/**
 * Apply credentials if present, otherwise enter SoftAP provisioning.
 * Stops an active portal when credentials become available (e.g. USB console).
 */
esp_err_t provisioning_apply_or_start(bridge_config_t *cfg);
