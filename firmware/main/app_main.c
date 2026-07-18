/*
 * T-Dongle-S3 multi-mode USB gadget
 *
 * Base function: USB CDC-NCM Ethernet gadget bridged to Wi-Fi STA (pico-usb-wifi
 * model, L2 + MAC adoption). On top of that, runtime-toggleable modes:
 *   - SD card as USB mass storage (MSC)
 *   - SD network share (WebDAV + web UI) over a STA-IP + NAT tether
 *   - network-controlled USB HID keyboard/mouse
 *
 * The optional USB functions (ACM console / MSC / HID) share the ESP32-S3's
 * limited USB IN endpoints; usb_gadget applies an LRU eviction policy. Enabling
 * storage/HID hosting switches USB networking from the L2 bridge to a NAT tether
 * so the ESP can hold its own DHCP IP and serve the LAN.
 */

#include <stdio.h>

#include "bridge.h"
#include "button.h"
#include "config_store.h"
#include "console.h"
#include "display.h"
#include "esp_log.h"
#include "hid.h"
#include "httpd_share.h"
#include "msc.h"
#include "nvs_flash.h"
#include "provisioning.h"
#include "sdcard.h"
#include "status_led.h"
#include "usb_gadget.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "T-Dongle-S3 multi-mode gadget starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    static bridge_config_t cfg;
    config_load(&cfg);

    /* Make sure the stored mode set fits the USB endpoint budget. */
    if (usb_gadget_sanitize(&cfg)) {
        config_save(&cfg);
    }

    const bool nat = config_nat_wanted(&cfg);
    const bool msc = config_usb_func_enabled(&cfg, USB_FUNC_MSC);
    const bool hid = config_usb_func_enabled(&cfg, USB_FUNC_HID);
    const bool acm = config_usb_func_enabled(&cfg, USB_FUNC_ACM);
    const bool share = config_share_enabled(&cfg);

    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(status_led_init());

    /* Always probe the SD card so the SD screen reflects reality and MSC/share
     * can be enabled at runtime for an already-inserted card (best-effort). */
    sdcard_init();

    /*
     * Boot order:
     * 1) Wi-Fi driver up (learn STA MAC; NAT mode also creates the STA netif)
     * 2) install the composite USB descriptor for the resolved mode set
     * 3) bind NCM (L2 bridge or NAT tether) + optional ACM / MSC / HID classes
     * 4) associate / provision
     */
    msc_register();  /* force-link the MSC class callbacks (referenced only by TinyUSB) */
    ESP_ERROR_CHECK(wifi_mgr_init(&cfg, nat));
    ESP_ERROR_CHECK(usb_gadget_init(&cfg));
    ESP_ERROR_CHECK(bridge_init(wifi_mgr_sta_mac(), nat));

    if (usb_gadget_func_active(USB_FUNC_ACM)) {
        ESP_ERROR_CHECK(console_init(&cfg));
    } else {
        ESP_LOGI(TAG, "CDC-ACM console disabled (endpoints used by MSC/HID); use the web UI");
    }

    if (usb_gadget_func_active(USB_FUNC_HID)) {
        ESP_ERROR_CHECK(hid_init());
    }

    /* SD ownership: MSC (USB host) takes priority; otherwise the web share owns
     * the FAT. The web UI can force-unmount to switch ownership at runtime. */
    if (sdcard_present()) {
        if (usb_gadget_func_active(USB_FUNC_MSC)) {
            sdcard_release_to_host();
        } else if (share) {
            sdcard_take_esp();
        }
    }

    display_task_start();
    status_led_task_start();
    ESP_ERROR_CHECK(button_init(&cfg));
    button_task_start();

    ESP_ERROR_CHECK(provisioning_apply_or_start(&cfg));

    /* Web file share / HID control server (needs the NAT tether IP path). */
    if ((share || hid) && !provisioning_is_active()) {
        httpd_share_start();
    }

    ESP_LOGI(TAG, "Modes: NCM(%s) ACM=%d MSC=%d HID=%d SHARE=%d",
             nat ? "NAT" : "L2", acm, msc, hid, share);
    ESP_LOGI(TAG, "BOOT button: tap=next screen, hold 2s=toggle mode, hold 5s + tap=factory reset");
}
