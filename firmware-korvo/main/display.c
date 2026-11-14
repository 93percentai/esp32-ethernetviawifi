#include "korvo_display.h"
#include "korvo_bridge.h"
#include "usb_ethernet.h"
#include "wifi_mgr.h"
#include "provisioning.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include <inttypes.h>

#if CONFIG_KORVO_LCD_ENABLED
static lv_obj_t *s_wifi, *s_usb, *s_client, *s_traffic, *s_setup;

static void setup_pressed(lv_event_t *event)
{
    (void)event;
    korvo_request_setup();
}

static void update(lv_timer_t *timer)
{
    (void)timer;
    wifi_mgr_status_t wifi; wifi_mgr_get_status(&wifi);
    usb_ethernet_status_t usb; usb_ethernet_status(&usb);
    korvo_bridge_status_t bridge; korvo_bridge_status(&bridge);
    bridge_stats_t stats; bridge_get_stats(&stats);
    provisioning_lcd_status_t setup; provisioning_get_lcd_status(&setup);
    lv_label_set_text_fmt(s_wifi, "Wi-Fi: %s  |  %d dBm", wifi.ssid[0] ? wifi.ssid : "unconfigured", wifi.rssi);
    lv_label_set_text_fmt(s_usb, "USB: %04x:%04x  |  Ethernet: %s\n%s",
        usb.vid, usb.pid, usb.link ? "cable connected" : "waiting", usb.detail);
    if (bridge.learned) {
        lv_label_set_text_fmt(s_client, "%s\nClient %02X:%02X:%02X:%02X:%02X:%02X",
            bridge.ready ? "Bridge forwarding" : "Connecting Wi-Fi as wired client...",
            bridge.client_mac[0], bridge.client_mac[1], bridge.client_mac[2],
            bridge.client_mac[3], bridge.client_mac[4], bridge.client_mac[5]);
    } else lv_label_set_text(s_client, "Waiting for one wired client (DHCP / ARP / IPv6)");
    lv_label_set_text_fmt(s_traffic,
        "Download %.1f KB/s  |  Upload %.1f KB/s\n"
        "Transferred %.2f / %.2f MB  |  Drops %" PRIu32 " / %" PRIu32 "\n"
        "USB errors %" PRIu32 "  |  Other client frames %" PRIu32,
        stats.rate_to_host_bps / 1000.0f, stats.rate_to_wifi_bps / 1000.0f,
        stats.bytes_to_host / 1000000.0, stats.bytes_to_wifi / 1000000.0,
        stats.drop_rx, stats.drop_tx, usb.errors, bridge.other_clients);
    lv_label_set_text_fmt(s_setup, "%s", setup.active ?
        "Wi-Fi setup: join " PROV_SOFTAP_SSID "\nOpen http://192.168.1.1" :
        "One Ethernet device, directly connected.\nUnplug its cable to learn a different device.");
}

static lv_obj_t *label(lv_obj_t *parent, int y)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_width(object, 750);
    lv_obj_align(object, LV_ALIGN_TOP_LEFT, 24, y);
    return object;
}
#endif

esp_err_t korvo_display_start(void)
{
#if CONFIG_KORVO_LCD_ENABLED
    if (!bsp_display_start()) return ESP_FAIL;
    /* This BSP has no controllable backlight. Do not call brightness APIs. */
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101923), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xe8f0fa), 0);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_18, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = label(screen, 18);
    lv_label_set_text(title, "Wi-Fi to Ethernet");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    s_wifi = label(screen, 62);
    s_usb = label(screen, 98);
    s_client = label(screen, 160);
    s_traffic = label(screen, 225);
    s_setup = label(screen, 338);
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_size(button, 230, 48);
    lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -24, -18);
    lv_obj_add_event_cb(button, setup_pressed, LV_EVENT_CLICKED, NULL);
    lv_obj_t *caption = lv_label_create(button);
    lv_label_set_text(caption, "Wi-Fi setup");
    lv_obj_center(caption);
    update(NULL);
    lv_timer_create(update, 1000, NULL);
    bsp_display_unlock();
#endif
    return ESP_OK;
}
