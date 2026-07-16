#pragma once

#include <stdbool.h>

#include "config_store.h"
#include "esp_err.h"

#define PROV_SOFTAP_SSID "ESP_WIFITOUSB_CONF"

/* SoftAP captive-portal IP (user-facing config URL). */
#define PROV_SOFTAP_IP_A 192
#define PROV_SOFTAP_IP_B 168
#define PROV_SOFTAP_IP_C 1
#define PROV_SOFTAP_IP_D 1

/**
 * When no SSID is configured: scan nearby networks, start SoftAP
 * ESP_WIFITOUSB_CONF at 192.168.1.1 with a captive-portal config page.
 * On a successful Wi-Fi test, credentials are saved and SoftAP stops.
 */
esp_err_t provisioning_start(bridge_config_t *cfg);

/** Tear down SoftAP / HTTP / DNS if active. Does not associate STA. */
void provisioning_stop(void);

bool provisioning_is_active(void);

/**
 * Apply credentials if present, otherwise enter SoftAP provisioning.
 * Stops an active portal when credentials become available (e.g. USB console).
 */
esp_err_t provisioning_apply_or_start(bridge_config_t *cfg);
