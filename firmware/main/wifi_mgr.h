#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "config_store.h"

typedef enum {
    WIFI_MGR_IDLE = 0,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_DISCONNECTED,
    WIFI_MGR_NO_AP,
    WIFI_MGR_BAD_AUTH,
    WIFI_MGR_SCANNING,
} wifi_mgr_state_t;

typedef struct {
    wifi_mgr_state_t state;
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t mac[6];
    uint8_t bssid[6];
} wifi_mgr_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
    uint8_t bssid[6];
} wifi_scan_result_t;

/* Start Wi-Fi driver and learn STA MAC; does not associate yet. */
esp_err_t wifi_mgr_init(bridge_config_t *cfg);
/* Apply active profile and (re)associate. Call after USB bridge is ready. */
esp_err_t wifi_mgr_apply(const bridge_config_t *cfg);
void wifi_mgr_get_status(wifi_mgr_status_t *out);
bool wifi_mgr_is_connected(void);
const uint8_t *wifi_mgr_sta_mac(void);

/* Blocking multi-pass scan; returns count written into out[] (max max_out). */
int wifi_mgr_scan(wifi_scan_result_t *out, int max_out);
