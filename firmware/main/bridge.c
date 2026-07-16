#include "bridge.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "class/net/net_device.h"
#include "tinyusb.h"
#include "tinyusb_net.h"

static const char *TAG = "bridge";

static bool s_wifi_up;
static uint8_t s_sta_mac[6];
static SemaphoreHandle_t s_stats_lock;
static bridge_stats_t s_stats;
static int64_t s_rate_window_us;
static uint64_t s_window_to_host;
static uint64_t s_window_to_wifi;

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

static void rate_tick(void)
{
    int64_t now = esp_timer_get_time();
    int64_t dt = now - s_rate_window_us;
    if (dt < 500000) {
        return;
    }
    float sec = (float)dt / 1000000.0f;
    float rh = (float)s_window_to_host / sec;
    float rw = (float)s_window_to_wifi / sec;
    /* EMA smoothing */
    s_stats.rate_to_host_bps = s_stats.rate_to_host_bps * 0.4f + rh * 0.6f;
    s_stats.rate_to_wifi_bps = s_stats.rate_to_wifi_bps * 0.4f + rw * 0.6f;
    s_window_to_host = 0;
    s_window_to_wifi = 0;
    s_rate_window_us = now;
}

static bool is_reflected(const uint8_t *frame, uint16_t len)
{
    if (len < 12) {
        return false;
    }
    /* Ethernet source MAC at offset 6 */
    return memcmp(frame + 6, s_sta_mac, 6) == 0;
}

static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_wifi_up) {
        stats_lock();
        s_stats.drop_tx++;
        stats_unlock();
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_internal_tx(WIFI_IF_STA, buffer, len);
    stats_lock();
    if (err == ESP_OK) {
        s_stats.bytes_to_wifi += len;
        s_stats.frames_to_wifi++;
        s_window_to_wifi += len;
    } else {
        s_stats.drop_tx++;
    }
    rate_tick();
    stats_unlock();
    return ESP_OK;
}

static void wifi_pkt_free(void *eb, void *ctx)
{
    (void)ctx;
    esp_wifi_internal_free_rx_buffer(eb);
}

static esp_err_t pkt_wifi2usb(void *buffer, uint16_t len, void *eb)
{
    if (is_reflected(buffer, len)) {
        stats_lock();
        s_stats.drop_refl++;
        stats_unlock();
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }

    /* sta2eth uses 100 ms; too-short timeouts drop DHCP bursts under USB FS load */
    if (tinyusb_net_send_sync(buffer, len, eb, pdMS_TO_TICKS(100)) != ESP_OK) {
        esp_wifi_internal_free_rx_buffer(eb);
        stats_lock();
        s_stats.drop_rx++;
        stats_unlock();
        return ESP_OK;
    }

    stats_lock();
    s_stats.bytes_to_host += len;
    s_stats.frames_to_host++;
    s_window_to_host += len;
    rate_tick();
    stats_unlock();
    return ESP_OK;
}

esp_err_t bridge_init(const uint8_t sta_mac[6])
{
    memcpy(s_sta_mac, sta_mac, 6);
    s_stats_lock = xSemaphoreCreateMutex();
    s_rate_window_us = esp_timer_get_time();

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = NULL,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = NULL,
        .hs_configuration_descriptor = NULL,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = NULL,
#endif
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB install failed");

    tinyusb_net_config_t net_config = {
        .on_recv_callback = usb_recv_callback,
        .free_tx_buffer = wifi_pkt_free,
        .user_context = NULL,
    };
    memcpy(net_config.mac_addr, sta_mac, 6);

    ESP_LOGI(TAG, "USB NCM MAC (host adopts STA): %02x:%02x:%02x:%02x:%02x:%02x",
             sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);

    ESP_RETURN_ON_ERROR(tinyusb_net_init(TINYUSB_USBDEV_0, &net_config), TAG, "NCM init failed");

    /*
     * Keep NCM link down until Wi-Fi associates. Espressif's updated tusb_ncm
     * example and IDFGH-17035 (Apple NCM DHCP) require this: hosts that DHCP
     * only on the NETWORK_CONNECTION notification otherwise race the bridge.
     */
    tud_network_link_state(0, false);
    s_wifi_up = false;
    return ESP_OK;
}

void bridge_set_wifi_up(bool up)
{
    if (up && !s_wifi_up) {
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, pkt_wifi2usb);
        tud_network_link_state(0, true);
        ESP_LOGI(TAG, "L2 bridge armed (wifi→usb), NCM link up");
    } else if (!up && s_wifi_up) {
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
        tud_network_link_state(0, false);
        ESP_LOGI(TAG, "L2 bridge disarmed, NCM link down");
    } else if (!up) {
        /* Idempotent down (e.g. before first association) */
        tud_network_link_state(0, false);
    }
    s_wifi_up = up;
}

void bridge_get_stats(bridge_stats_t *out)
{
    stats_lock();
    rate_tick();
    *out = s_stats;
    stats_unlock();
}

void bridge_reset_stats(void)
{
    stats_lock();
    memset(&s_stats, 0, sizeof(s_stats));
    s_window_to_host = 0;
    s_window_to_wifi = 0;
    s_rate_window_us = esp_timer_get_time();
    stats_unlock();
}

void bridge_format_bytes(uint64_t bytes, char *out, size_t out_len)
{
    if (bytes < 1000ULL) {
        snprintf(out, out_len, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1000ULL * 1000ULL) {
        snprintf(out, out_len, "%.1f KB", bytes / 1000.0);
    } else if (bytes < 1000ULL * 1000ULL * 1000ULL) {
        snprintf(out, out_len, "%.2f MB", bytes / 1000000.0);
    } else {
        snprintf(out, out_len, "%.2f GB", bytes / 1000000000.0);
    }
}

void bridge_format_rate(float bps, char *out, size_t out_len)
{
    if (bps < 1000.0f) {
        snprintf(out, out_len, "%.0f B/s", bps);
    } else if (bps < 1000.0f * 1000.0f) {
        snprintf(out, out_len, "%.1f KB/s", bps / 1000.0f);
    } else {
        snprintf(out, out_len, "%.2f MB/s", bps / 1000000.0f);
    }
}
