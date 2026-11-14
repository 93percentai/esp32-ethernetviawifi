#include "korvo_bridge.h"
#include "diagnostics.h"
#include "korvo_display.h"
#include "usb_ethernet.h"
#include "config_store.h"
#include "wifi_mgr.h"
#include "provisioning.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

void app_main(void)
{
    diagnostics_init();
    ESP_LOGI("main", "ESP32-S31-Korvo-1 USB Ethernet bridge: ECM / NCM / RNDIS / native drivers");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    static bridge_config_t cfg;
    config_load(&cfg);
    ESP_ERROR_CHECK(wifi_mgr_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(bridge_init(wifi_mgr_sta_mac()));
    ESP_ERROR_CHECK(usb_ethernet_start());
    /* The common portal owns only an AP netif. The bridged STA has no local IP
     * or DHCP client; DHCP comes from the directly connected Ethernet device. */
    err = korvo_display_start();
    if (err != ESP_OK) ESP_LOGW("main", "LCD unavailable: %s; bridge continues", esp_err_to_name(err));
    ESP_ERROR_CHECK(provisioning_apply_or_start(&cfg));
    ESP_ERROR_CHECK(korvo_bridge_start(&cfg));
    for (;;) {
        diagnostics_serial_status();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
