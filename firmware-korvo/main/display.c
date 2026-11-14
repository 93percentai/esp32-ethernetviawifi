#include "korvo_display.h"
#include "korvo_bridge.h"
#include "usb_ethernet.h"
#include "diagnostics.h"
#include "wifi_mgr.h"
#include "provisioning.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <inttypes.h>
#include <stdio.h>

#if CONFIG_KORVO_LCD_ENABLED
static lv_obj_t *s_summary,*s_details,*s_logs,*s_log_note,*s_setup;
static char s_log_text[4096];
static void setup_pressed(lv_event_t *event) { (void)event; korvo_request_setup(); }
static const char *wifi_state(wifi_mgr_state_t state)
{
    static const char *const labels[]={"idle","connecting","associated","disconnected","AP not found","authentication failed","scanning","Wi-Fi setup"};
    return (unsigned)state<sizeof(labels)/sizeof(*labels)?labels[state]:"unknown";
}
static void update(lv_timer_t *timer)
{
    (void)timer;
    wifi_mgr_status_t wifi; wifi_mgr_get_status(&wifi);
    usb_ethernet_status_t usb; usb_ethernet_status(&usb);
    korvo_bridge_status_t bridge; korvo_bridge_status(&bridge);
    bridge_stats_t stats; bridge_get_stats(&stats);
    provisioning_lcd_status_t setup; provisioning_get_lcd_status(&setup);
    int64_t now=esp_timer_get_time();
    lv_label_set_text_fmt(s_summary,
        "%s\nWi-Fi: %s | %s | %d dBm | ch %u\n"
        "USB %04X:%04X | %s | config %u/%u\n%s\n"
        "Cable: %s | %"PRIu32" Mbit/s | %s\n"
        "Wired client: %s %02X:%02X:%02X:%02X:%02X:%02X\n"
        "Download %.1f KB/s   Upload %.1f KB/s\n"
        "Transferred %.2f / %.2f MB | Drops %"PRIu32" / %"PRIu32"\n%s",
        bridge.ready?"BRIDGE FORWARDING":usb.ready?"Waiting for cable / client / Wi-Fi":"USB SETUP / see Diagnostics and Logs",
        wifi.ssid[0]?wifi.ssid:"unconfigured",wifi_state(wifi.state),wifi.rssi,wifi.channel,
        usb.vid,usb.pid,usb_net_driver_name(usb.driver),usb.configuration,usb.configurations,usb.detail,
        usb.link?"up":"down",usb.speed_mbps,usb.full_duplex?"full duplex":"duplex unknown / half",
        bridge.learned?"":"waiting",bridge.client_mac[0],bridge.client_mac[1],bridge.client_mac[2],bridge.client_mac[3],bridge.client_mac[4],bridge.client_mac[5],
        stats.rate_to_host_bps/1000.0f,stats.rate_to_wifi_bps/1000.0f,stats.bytes_to_host/1000000.0,stats.bytes_to_wifi/1000000.0,
        stats.drop_rx,stats.drop_tx,setup.active?"Join "PROV_SOFTAP_SSID" and open http://192.168.1.1":"One directly connected client. Unplug cable/USB to learn another.");
    lv_label_set_text_fmt(s_details,
        "Uptime %"PRId64" s | USB stage: %s\nProduct: %s\n"
        "USB %s | address %u | revision bcd=%04X chip=%04X\n"
        "Control interface %u | data %u alt %u\nEndpoints IN %02X (%u) OUT %02X (%u) notify %02X\n"
        "Adapter MAC %02X:%02X:%02X:%02X:%02X:%02X\n"
        "Station MAC %02X:%02X:%02X:%02X:%02X:%02X\n"
        "Wi-Fi %s | disconnects %"PRIu32" | last reason %u\n"
        "USB control count/fail %"PRIu32"/%"PRIu32" | resets %"PRIu32" | timeouts %"PRIu32"\n"
        "Last EP0: %02X/%02X value=%04X index=%04X length=%u\n"
        "Last transfer ep=%02X status=%d | submit error=%d\n"
        "USB RX blocks/frames/bad %"PRIu32"/%"PRIu32"/%"PRIu32" | TX blocks %"PRIu32"\n"
        "TX queue high-water %"PRIu32"/24 | full drops %"PRIu32" | stale %"PRIu32"\n"
        "NCM/RNDIS block limits IN/OUT %"PRIu32"/%"PRIu32"\n"
        "ARP Ethernet/Wi-Fi %"PRIu32"/%"PRIu32" | DHCP %"PRIu32"/%"PRIu32"\n"
        "MAC adoption attempts/failures %"PRIu32"/%"PRIu32" | Wi-Fi TX error %d\n"
        "Other-client frames %"PRIu32" | invalid Ethernet %"PRIu32"\n"
        "USB last RX/TX age %"PRId64"/%"PRId64" ms\n"
        "Internal heap %u KB | minimum %u KB | PSRAM %u KB\n"
        "Last USB error: %s",
        now/1000000,usb.stage,usb.product,usb.usb_speed==2?"HS 480 Mbit/s":usb.usb_speed==1?"FS 12 Mbit/s":"unknown speed",
        usb.address,usb.bcd_device,usb.revision,usb.control_interface,usb.data_interface,usb.data_alt,
        usb.bulk_in,usb.in_mps,usb.bulk_out,usb.out_mps,usb.notification,
        usb.adapter_mac[0],usb.adapter_mac[1],usb.adapter_mac[2],usb.adapter_mac[3],usb.adapter_mac[4],usb.adapter_mac[5],
        wifi.mac[0],wifi.mac[1],wifi.mac[2],wifi.mac[3],wifi.mac[4],wifi.mac[5],wifi_state(wifi.state),wifi.disconnects,wifi.last_disconnect_reason,
        usb.control_requests,usb.control_failures,usb.reconfigurations,usb.timeouts,
        usb.request_type,usb.request,usb.request_value,usb.request_index,usb.request_length,
        usb.last_endpoint,usb.last_transfer_status,usb.last_submit_error,
        usb.rx_transfers,usb.rx_frames,usb.rx_invalid,usb.tx_transfers,usb.queue_high_water,usb.queue_drops,usb.stale_drops,
        usb.ntb_in,usb.ntb_out,bridge.ethernet_arp,bridge.wifi_arp,bridge.ethernet_dhcp,bridge.wifi_dhcp,
        bridge.adoption_attempts,bridge.adoption_failures,bridge.last_wifi_tx_error,bridge.other_clients,bridge.invalid_frames,
        usb.last_rx_us?(now-usb.last_rx_us)/1000:-1,usb.last_tx_us?(now-usb.last_tx_us)/1000:-1,
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)/1024),(unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)/1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024),usb.last_error[0]?usb.last_error:"none");
    uint64_t overwritten,truncated;
    diagnostics_log_tail(s_log_text,sizeof(s_log_text),&overwritten,&truncated);
    lv_label_set_text(s_logs,s_log_text);
    lv_label_set_text_fmt(s_log_note,"Recent app logs (resets on boot). Overwritten %"PRIu64" / truncated %"PRIu64" bytes. Scroll to inspect.",overwritten,truncated);
    lv_label_set_text(s_setup,setup.active?"Wi-Fi setup active: http://192.168.1.1":"Wi-Fi setup");
}
static lv_obj_t *label(lv_obj_t *parent)
{
    lv_obj_t *object=lv_label_create(parent); lv_obj_set_width(object,730);
    lv_label_set_long_mode(object,LV_LABEL_LONG_WRAP); return object;
}
#endif
esp_err_t korvo_display_start(void)
{
#if CONFIG_KORVO_LCD_ENABLED
    if(!bsp_display_start()) return ESP_FAIL;
    if(!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    /* S31 BSP has no supported backlight control call. */
    lv_obj_t *screen=lv_screen_active(); lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x101923),0);
    lv_obj_set_style_text_color(screen,lv_color_hex(0xe8f0fa),0);
    lv_obj_set_style_text_font(screen,&lv_font_montserrat_18,0);
    lv_obj_t *tabs=lv_tabview_create(screen); lv_obj_set_size(tabs,800,414); lv_tabview_set_tab_bar_size(tabs,42);
    lv_obj_t *overview=lv_tabview_add_tab(tabs,"Overview"); s_summary=label(overview);
    lv_obj_t *details=lv_tabview_add_tab(tabs,"Diagnostics"); s_details=label(details);
    lv_obj_t *logs=lv_tabview_add_tab(tabs,"Logs"); lv_obj_set_flex_flow(logs,LV_FLEX_FLOW_COLUMN);
    s_log_note=label(logs); s_logs=label(logs); lv_obj_set_style_text_font(s_logs,&lv_font_montserrat_14,0);
    lv_obj_t *button=lv_button_create(screen); lv_obj_set_size(button,740,46); lv_obj_align(button,LV_ALIGN_BOTTOM_MID,0,-10);
    lv_obj_add_event_cb(button,setup_pressed,LV_EVENT_CLICKED,NULL); s_setup=lv_label_create(button); lv_obj_center(s_setup);
    update(NULL); lv_timer_create(update,1000,NULL); bsp_display_unlock();
#endif
    return ESP_OK;
}
