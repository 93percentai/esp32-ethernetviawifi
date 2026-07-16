/*
 * esp32-ethernetviawifi
 *
 * Port of the pico-usb-wifi idea to LilyGO T-Dongle-S3:
 * USB CDC-NCM ethernet gadget + transparent L2 bridge onto Wi-Fi STA,
 * with CDC-ACM management console and ST7735 status LCD.
 *
 * Architecture mirrors pico-usb-wifi / Espressif tusb_ncm:
 * the host adopts the STA MAC; frames are forwarded verbatim with
 * esp_wifi_internal_tx / esp_wifi_internal_reg_rxcb.
 */

#include <stdio.h>

#include "bridge.h"
#include "button.h"
#include "config_store.h"
#include "console.h"
#include "display.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "provisioning.h"
#include "status_led.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "esp32-ethernetviawifi starting on T-Dongle-S3");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    static bridge_config_t cfg;
    config_load(&cfg);

    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(status_led_init());

    /*
     * Order matches Espressif tusb_ncm / sta2eth USB path:
     * 1) start Wi-Fi driver (MAC known, no associate yet)
     * 2) bring up USB NCM with that MAC, link forced down
     * 3) CDC console
     * 4) then associate — link-up notifies the host when the bridge is ready
     */
    ESP_ERROR_CHECK(wifi_mgr_init(&cfg));
    ESP_ERROR_CHECK(bridge_init(wifi_mgr_sta_mac()));
    ESP_ERROR_CHECK(console_init(&cfg));

    /* Start HUD before provisioning so SoftAP scan/portal status is live. */
    display_task_start();
    status_led_task_start();
    ESP_ERROR_CHECK(button_init(&cfg));
    button_task_start();

    ESP_ERROR_CHECK(provisioning_apply_or_start(&cfg));

    if (provisioning_is_active()) {
        ESP_LOGI(TAG, "Ready. Join SoftAP '%s' and open http://192.168.1.1 "
                      "(or use the USB CDC console).",
                 PROV_SOFTAP_SSID);
    } else {
        ESP_LOGI(TAG, "Ready. Provision via USB CDC serial console if needed.");
    }
    ESP_LOGI(TAG, "BOOT button: press once for reset prompt, again within 5s to clear Wi-Fi.");
}
