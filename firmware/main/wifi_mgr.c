#include "wifi_mgr.h"

#include <stdlib.h>
#include <string.h>

#include "bridge.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_mgr";

static bridge_config_t *s_cfg;
static wifi_mgr_status_t s_status;
static uint8_t s_sta_mac[6];
static EventGroupHandle_t s_wifi_events;
static bool s_suppress_bridge;

#define WIFI_SCAN_DONE_BIT   BIT0
#define WIFI_STA_OK_BIT      BIT1
#define WIFI_STA_FAIL_BIT    BIT2

static void update_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_status.rssi = ap.rssi;
        s_status.channel = ap.primary;
        memcpy(s_status.bssid, ap.bssid, 6);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        if (s_status.state == WIFI_MGR_PROVISIONING) {
            break;
        }
        s_status.state = WIFI_MGR_CONNECTING;
        if (config_active_ssid(s_cfg)[0]) {
            esp_wifi_connect();
        } else {
            s_status.state = WIFI_MGR_IDLE;
        }
        break;

    case WIFI_EVENT_STA_CONNECTED: {
        wifi_event_sta_connected_t *ev = data;
        if (s_status.state != WIFI_MGR_PROVISIONING) {
            s_status.state = WIFI_MGR_CONNECTED;
        }
        memcpy(s_status.ssid, ev->ssid, sizeof(s_status.ssid));
        s_status.ssid[sizeof(s_status.ssid) - 1] = '\0';
        s_status.channel = ev->channel;
        memcpy(s_status.bssid, ev->bssid, 6);
        update_rssi();
        if (!s_suppress_bridge) {
            bridge_set_wifi_up(true);
        }
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_STA_OK_BIT);
        }
        ESP_LOGI(TAG, "Associated to '%s' ch=%u", s_status.ssid, s_status.channel);
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *ev = data;
        if (!s_suppress_bridge) {
            bridge_set_wifi_up(false);
        }
        ESP_LOGW(TAG, "Disconnected reason=%u", ev->reason);

        if (ev->reason == WIFI_REASON_NO_AP_FOUND) {
            if (s_status.state != WIFI_MGR_PROVISIONING) {
                s_status.state = WIFI_MGR_NO_AP;
            }
        } else if (ev->reason == WIFI_REASON_AUTH_FAIL ||
                   ev->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                   ev->reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
            if (s_status.state != WIFI_MGR_PROVISIONING) {
                s_status.state = WIFI_MGR_BAD_AUTH;
            }
        } else if (s_status.state != WIFI_MGR_PROVISIONING &&
                   s_status.state != WIFI_MGR_SCANNING) {
            s_status.state = WIFI_MGR_DISCONNECTED;
        }

        s_status.rssi = 0;
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_STA_FAIL_BIT);
        }

        if (!s_suppress_bridge &&
            config_active_ssid(s_cfg)[0] &&
            s_status.state != WIFI_MGR_BAD_AUTH &&
            s_status.state != WIFI_MGR_PROVISIONING &&
            s_status.state != WIFI_MGR_SCANNING) {
            s_status.state = WIFI_MGR_CONNECTING;
            esp_wifi_connect();
        }
        break;
    }

    case WIFI_EVENT_SCAN_DONE:
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_SCAN_DONE_BIT);
        }
        break;

    default:
        break;
    }
}

esp_err_t wifi_mgr_init(bridge_config_t *cfg)
{
    s_cfg = cfg;
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = WIFI_MGR_IDLE;
    s_suppress_bridge = false;
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_get_mac(WIFI_IF_STA, s_sta_mac);
    memcpy(s_status.mac, s_sta_mac, 6);
    ESP_LOGI(TAG, "STA MAC %02x:%02x:%02x:%02x:%02x:%02x",
             s_sta_mac[0], s_sta_mac[1], s_sta_mac[2],
             s_sta_mac[3], s_sta_mac[4], s_sta_mac[5]);

    /* Association is deferred until USB NCM is up (see app_main). */
    (void)cfg;
    return ESP_OK;
}

esp_err_t wifi_mgr_apply(const bridge_config_t *cfg)
{
    const char *ssid = config_active_ssid(cfg);
    const char *pass = config_active_pass(cfg);

    esp_wifi_disconnect();
    if (!s_suppress_bridge) {
        bridge_set_wifi_up(false);
    }

    if (!ssid[0]) {
        s_status.state = WIFI_MGR_IDLE;
        s_status.ssid[0] = '\0';
        ESP_LOGI(TAG, "No active profile");
        return ESP_OK;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    /* Match pico-usb-wifi: open or WPA2/WPA3-SAE transition */
    if (pass[0]) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
        wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    /* Ensure SoftAP from a prior portal session is not left advertising. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1);
    s_status.state = WIFI_MGR_CONNECTING;
    ESP_LOGI(TAG, "Connecting to '%s'", ssid);
    return esp_wifi_connect();
}

void wifi_mgr_get_status(wifi_mgr_status_t *out)
{
    if (s_status.state == WIFI_MGR_CONNECTED) {
        update_rssi();
    }
    *out = s_status;
}

bool wifi_mgr_is_connected(void)
{
    return s_status.state == WIFI_MGR_CONNECTED;
}

const uint8_t *wifi_mgr_sta_mac(void)
{
    return s_sta_mac;
}

void wifi_mgr_set_suppress_bridge(bool suppress)
{
    s_suppress_bridge = suppress;
    if (suppress) {
        bridge_set_wifi_up(false);
    }
}

void wifi_mgr_set_state(wifi_mgr_state_t state)
{
    s_status.state = state;
}

static void fill_sta_config(wifi_config_t *wifi_config, const char *ssid, const char *pass)
{
    memset(wifi_config, 0, sizeof(*wifi_config));
    strncpy((char *)wifi_config->sta.ssid, ssid, sizeof(wifi_config->sta.ssid) - 1);
    if (pass) {
        strncpy((char *)wifi_config->sta.password, pass, sizeof(wifi_config->sta.password) - 1);
    }
    if (pass && pass[0]) {
        wifi_config->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config->sta.pmf_cfg.capable = true;
        wifi_config->sta.pmf_cfg.required = false;
        wifi_config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    } else {
        wifi_config->sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
}

esp_err_t wifi_mgr_test_connect(const char *ssid, const char *pass, int timeout_ms)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config;
    fill_sta_config(&wifi_config, ssid, pass ? pass : "");

    esp_wifi_disconnect();
    /* Drain the disconnect event so it does not look like a failed test. */
    vTaskDelay(pdMS_TO_TICKS(150));
    xEventGroupClearBits(s_wifi_events, WIFI_STA_OK_BIT | WIFI_STA_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1);
    ESP_LOGI(TAG, "Test-connect to '%s'", ssid);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_STA_OK_BIT | WIFI_STA_FAIL_BIT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_STA_OK_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_STA_FAIL_BIT) {
        return ESP_FAIL;
    }
    esp_wifi_disconnect();
    return ESP_ERR_TIMEOUT;
}

int wifi_mgr_scan(wifi_scan_result_t *out, int max_out, bool reassociate)
{
    if (!out || max_out <= 0) {
        return 0;
    }

    wifi_mgr_state_t prev = s_status.state;
    s_status.state = WIFI_MGR_SCANNING;
    if (!s_suppress_bridge) {
        bridge_set_wifi_up(false);
    }
    esp_wifi_disconnect();

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    xEventGroupClearBits(s_wifi_events, WIFI_SCAN_DONE_BIT);
    if (esp_wifi_scan_start(&scan_cfg, false) != ESP_OK) {
        s_status.state = prev;
        if (reassociate) {
            wifi_mgr_apply(s_cfg);
        }
        return 0;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_SCAN_DONE_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(bits & WIFI_SCAN_DONE_BIT)) {
        esp_wifi_scan_stop();
        s_status.state = prev;
        if (reassociate) {
            wifi_mgr_apply(s_cfg);
        }
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        s_status.state = prev;
        if (reassociate) {
            wifi_mgr_apply(s_cfg);
        }
        return 0;
    }

    wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!records) {
        s_status.state = prev;
        if (reassociate) {
            wifi_mgr_apply(s_cfg);
        }
        return 0;
    }

    uint16_t n = ap_count;
    esp_wifi_scan_get_ap_records(&n, records);

    int written = 0;
    for (uint16_t i = 0; i < n && written < max_out; i++) {
        /* Deduplicate by SSID, keep strongest */
        int existing = -1;
        for (int j = 0; j < written; j++) {
            if (strcmp(out[j].ssid, (char *)records[i].ssid) == 0) {
                existing = j;
                break;
            }
        }
        if (existing >= 0) {
            if (records[i].rssi > out[existing].rssi) {
                out[existing].rssi = records[i].rssi;
                out[existing].authmode = records[i].authmode;
                memcpy(out[existing].bssid, records[i].bssid, 6);
            }
            continue;
        }
        strncpy(out[written].ssid, (char *)records[i].ssid, sizeof(out[written].ssid) - 1);
        out[written].rssi = records[i].rssi;
        out[written].authmode = records[i].authmode;
        memcpy(out[written].bssid, records[i].bssid, 6);
        written++;
    }

    free(records);
    s_status.state = prev;
    if (reassociate) {
        wifi_mgr_apply(s_cfg);
    }
    return written;
}
