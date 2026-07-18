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
    WIFI_MGR_PROVISIONING,
} wifi_mgr_state_t;

typedef struct {
    wifi_mgr_state_t state;
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t mac[6];
    uint8_t bssid[6];
    bool nat_mode;        /* true when running as NAT tether (ESP holds an IP) */
    bool has_ip;          /* STA obtained a DHCP IP (NAT mode only) */
    uint32_t sta_ip;      /* IPv4 address in network byte order (NAT mode) */
} wifi_mgr_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
    uint8_t bssid[6];
} wifi_scan_result_t;

/* Start Wi-Fi driver and learn STA MAC; does not associate yet.
 * nat_mode: create a default STA esp_netif so the ESP obtains its own DHCP IP
 * (needed for the NAT tether + web/WebDAV/HID hosting). */
esp_err_t wifi_mgr_init(bridge_config_t *cfg, bool nat_mode);
/* Apply active profile and (re)associate. Call after USB bridge is ready. */
esp_err_t wifi_mgr_apply(const bridge_config_t *cfg);
void wifi_mgr_get_status(wifi_mgr_status_t *out);
bool wifi_mgr_is_connected(void);
const uint8_t *wifi_mgr_sta_mac(void);

/* When true, association events do not toggle the USB NCM link. */
void wifi_mgr_set_suppress_bridge(bool suppress);

/* Force a status label (used while SoftAP captive portal is up). */
void wifi_mgr_set_state(wifi_mgr_state_t state);

/*
 * Blocking scan. If reassociate is true, restores the previous association
 * attempt afterward (normal console scan). If false, leaves radio alone.
 * Returns count written into out[] (max max_out).
 */
int wifi_mgr_scan(wifi_scan_result_t *out, int max_out, bool reassociate);

/*
 * Configure STA credentials and wait for association (or failure).
 * Used by SoftAP provisioning to test a network before saving.
 * Returns ESP_OK on association, ESP_ERR_TIMEOUT / ESP_FAIL otherwise.
 */
esp_err_t wifi_mgr_test_connect(const char *ssid, const char *pass, int timeout_ms);
