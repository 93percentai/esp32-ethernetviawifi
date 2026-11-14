#include "korvo_bridge.h"
#include "ecm_protocol.h"
#include "usb_ethernet.h"
#include "provisioning.h"
#include "wifi_mgr.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_private/wifi.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bridge_stats_t s_stats;
static korvo_bridge_status_t s_client;
static bool s_wifi_up, s_ethernet_up, s_mac_matches;
static int64_t s_rate_us;
static uint64_t s_last_down, s_last_up;
static atomic_bool s_setup_request;
static bridge_config_t *s_cfg;

static bool is_dhcp(const uint8_t *f, size_t n)
{
    if (n < 42 || f[12] != 8 || f[13] != 0 || (f[14] >> 4) != 4 || f[23] != 17) return false;
    size_t ihl = (f[14] & 15) * 4;
    if (ihl < 20 || n < 14 + ihl + 8 || (f[20] & 0x3f) || f[21]) return false;
    const uint8_t *udp = f + 14 + ihl;
    unsigned from = (udp[0] << 8) | udp[1], to = (udp[2] << 8) | udp[3];
    return (from == 67 && to == 68) || (from == 68 && to == 67);
}

static void update_ready(void)
{
    s_client.ready = s_client.learned && s_mac_matches && s_wifi_up && s_ethernet_up;
}

static esp_err_t wifi_receive(void *buffer, uint16_t length, void *eb)
{
    const uint8_t *frame = buffer;
    portENTER_CRITICAL(&s_lock);
    if (is_dhcp(frame, length)) ++s_client.wifi_dhcp;
    if (length >= 14 && frame[12] == 8 && frame[13] == 6) ++s_client.wifi_arp;
    bool ready = s_client.ready;
    bool reflected = length >= 12 && !memcmp(frame + 6, s_client.client_mac, 6);
    bool destined = length >= 14 && ((frame[0] & 1) || !memcmp(frame, s_client.client_mac, 6));
    if (reflected) ++s_stats.drop_refl;
    portEXIT_CRITICAL(&s_lock);
    if (!reflected) {
        esp_err_t err = ready && destined && bridge_frame_valid(frame, length) ?
                        usb_ethernet_send(frame, length) : ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock); ++s_stats.drop_rx; portEXIT_CRITICAL(&s_lock);
        }
    }
    /* USB owns a copy; release the Wi-Fi driver buffer exactly once on every path. */
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

esp_err_t bridge_init(const uint8_t sta_mac[6])
{
    (void)sta_mac;
    s_rate_us = esp_timer_get_time();
    return ESP_OK;
}

void bridge_set_wifi_up(bool up)
{
    portENTER_CRITICAL(&s_lock);
    s_wifi_up = up;
    update_ready();
    portEXIT_CRITICAL(&s_lock);
    /* Kept registered while down: queued driver RX buffers still get freed. */
    esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_receive);
    if (!up) usb_ethernet_flush();
}

void korvo_bridge_ethernet_link(bool up)
{
    portENTER_CRITICAL(&s_lock);
    s_ethernet_up = up;
    if (!up) {
        s_client.learned = false;
        s_mac_matches = false;
        memset(s_client.client_mac, 0, 6);
    }
    update_ready();
    portEXIT_CRITICAL(&s_lock);
}

void korvo_bridge_receive(const uint8_t *frame, size_t length)
{
    if (!bridge_frame_valid(frame, length)) {
        portENTER_CRITICAL(&s_lock); ++s_client.invalid_frames; portEXIT_CRITICAL(&s_lock);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    if (is_dhcp(frame, length)) ++s_client.ethernet_dhcp;
    if (frame[12] == 8 && frame[13] == 6) ++s_client.ethernet_arp;
    if (!s_client.learned && s_ethernet_up && bridge_frame_can_learn(frame, length)) {
        memcpy(s_client.client_mac, frame + 6, 6);
        s_client.learned = true;
        s_mac_matches = false;
    }
    bool same = s_client.learned && !memcmp(frame + 6, s_client.client_mac, 6);
    bool ready = s_client.ready && same;
    if (!same) ++s_client.other_clients;
    if (!ready) ++s_stats.drop_tx;
    portEXIT_CRITICAL(&s_lock);
    if (!ready) return;
    esp_err_t err = esp_wifi_internal_tx(WIFI_IF_STA, (void *)frame, length);
    portENTER_CRITICAL(&s_lock);
    s_client.last_wifi_tx_error = err;
    if (err == ESP_OK) { s_stats.bytes_to_wifi += length; ++s_stats.frames_to_wifi; }
    else ++s_stats.drop_tx;
    portEXIT_CRITICAL(&s_lock);
}

void korvo_bridge_tx_result(bool success, size_t length)
{
    portENTER_CRITICAL(&s_lock);
    if (success) { s_stats.bytes_to_host += length; ++s_stats.frames_to_host; }
    else ++s_stats.drop_rx;
    portEXIT_CRITICAL(&s_lock);
}

void korvo_bridge_status(korvo_bridge_status_t *out)
{
    portENTER_CRITICAL(&s_lock); *out = s_client; portEXIT_CRITICAL(&s_lock);
}

void bridge_get_stats(bridge_stats_t *out)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (now - s_rate_us >= 500000) {
        float seconds = (now - s_rate_us) / 1000000.0f;
        s_stats.rate_to_host_bps = (s_stats.bytes_to_host - s_last_down) / seconds;
        s_stats.rate_to_wifi_bps = (s_stats.bytes_to_wifi - s_last_up) / seconds;
        s_last_down = s_stats.bytes_to_host; s_last_up = s_stats.bytes_to_wifi; s_rate_us = now;
    }
    *out = s_stats;
    portEXIT_CRITICAL(&s_lock);
}

void bridge_reset_stats(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_stats, 0, sizeof(s_stats));
    s_last_down = s_last_up = 0; s_rate_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
}

void bridge_format_bytes(uint64_t bytes, char *out, size_t capacity)
{
    if (bytes < 1000000) snprintf(out, capacity, "%.1f KB", bytes / 1000.0);
    else snprintf(out, capacity, "%.2f MB", bytes / 1000000.0);
}

void bridge_format_rate(float bytes, char *out, size_t capacity)
{
    snprintf(out, capacity, "%.1f KB/s", bytes / 1000.0f);
}

void korvo_request_setup(void) { atomic_store(&s_setup_request, true); }

static void control_task(void *arg)
{
    (void)arg;
    int64_t disconnected_since = esp_timer_get_time();
    int64_t last_adoption = 0;
    for (;;) {
        bool setup = atomic_exchange(&s_setup_request, false);
        if (wifi_mgr_is_connected()) disconnected_since = esp_timer_get_time();
        if (setup || (!provisioning_is_active() && esp_timer_get_time() - disconnected_since > 30000000)) {
            provisioning_start(s_cfg);
            disconnected_since = esp_timer_get_time();
        }
        korvo_bridge_status_t client; korvo_bridge_status(&client);
        portENTER_CRITICAL(&s_lock);
        bool matched = s_mac_matches;
        portEXIT_CRITICAL(&s_lock);
        if (client.learned && !provisioning_is_active() && wifi_mgr_is_connected() &&
            !matched && esp_timer_get_time() - last_adoption > 2000000) {
            last_adoption = esp_timer_get_time();
            ESP_LOGI("bridge", "Adopting wired client MAC %02x:%02x:%02x:%02x:%02x:%02x",
                client.client_mac[0], client.client_mac[1], client.client_mac[2],
                client.client_mac[3], client.client_mac[4], client.client_mac[5]);
            portENTER_CRITICAL(&s_lock); ++s_client.adoption_attempts; portEXIT_CRITICAL(&s_lock);
            esp_err_t err = wifi_mgr_adopt_mac(client.client_mac);
            portENTER_CRITICAL(&s_lock);
            if (err != ESP_OK) ++s_client.adoption_failures;
            if (err == ESP_OK && s_client.learned && !memcmp(s_client.client_mac, client.client_mac, 6)) {
                s_mac_matches = true; update_ready();
            }
            portEXIT_CRITICAL(&s_lock);
            if (err != ESP_OK) ESP_LOGW("bridge", "MAC adoption failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

esp_err_t korvo_bridge_start(bridge_config_t *cfg)
{
    s_cfg = cfg;
    return xTaskCreate(control_task, "bridge_control", 6144, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
