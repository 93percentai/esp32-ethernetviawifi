#include "diagnostics.h"
#include "log_buffer.h"
#include "usb_ethernet.h"
#include "korvo_bridge.h"
#include "wifi_mgr.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static char s_storage[16384];
static log_buffer_t s_log;
static portMUX_TYPE s_log_lock=portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_uart=vprintf;
static uint64_t s_truncated;
static unsigned s_ansi;

static int capture(const char *format,va_list args)
{
    char text[384]; va_list copy; va_copy(copy,args);
    int wanted=vsnprintf(text,sizeof(text),format,copy); va_end(copy);
    int result=s_uart(format,args);
    if(wanted<=0) return result;
    size_t size=(unsigned)wanted<sizeof(text)?(unsigned)wanted:sizeof(text)-1;
    portENTER_CRITICAL(&s_log_lock);
    if((unsigned)wanted>size) s_truncated+=(unsigned)wanted-size;
    for(size_t i=0;i<size;++i) {
        unsigned char c=text[i];
        if(s_ansi==1) { s_ansi=c=='['?2:0; continue; }
        if(s_ansi==2) { if(c>=0x40 && c<=0x7e) s_ansi=0; continue; }
        if(c==27) { s_ansi=1; continue; }
        if(c=='\n' || c=='\t' || c>=32) log_buffer_append(&s_log,(char *)&text[i],1);
    }
    portEXIT_CRITICAL(&s_log_lock);
    return result;
}
void diagnostics_init(void)
{
    log_buffer_init(&s_log,s_storage,sizeof(s_storage));
    s_uart=esp_log_set_vprintf(capture);
}
size_t diagnostics_log_tail(char *out,size_t capacity,uint64_t *overwritten,uint64_t *truncated)
{
    portENTER_CRITICAL(&s_log_lock);
    size_t n=log_buffer_copy(&s_log,out,capacity);
    if(overwritten) *overwritten=s_log.overwritten;
    if(truncated) *truncated=s_truncated;
    portEXIT_CRITICAL(&s_log_lock);
    if(n && n+1==capacity) {
        char *line=memchr(out,'\n',n);
        if(line) { size_t prefix=line-out+1; memmove(out,out+prefix,n-prefix+1); n-=prefix; }
    }
    return n;
}
void diagnostics_serial_status(void)
{
    usb_ethernet_status_t usb; usb_ethernet_status(&usb);
    korvo_bridge_status_t client; korvo_bridge_status(&client);
    bridge_stats_t stats; bridge_get_stats(&stats);
    wifi_mgr_status_t wifi; wifi_mgr_get_status(&wifi);
    int64_t now=esp_timer_get_time();
    ESP_LOGI("status","uptime=%"PRId64"s WiFi state=%d SSID='%s' RSSI=%d channel=%u disconnects=%"PRIu32" last_reason=%u",now/1000000,wifi.state,wifi.ssid,wifi.rssi,wifi.channel,wifi.disconnects,wifi.last_disconnect_reason);
    ESP_LOGI("status","USB %04x:%04x config=%u/%u %s stage='%s' ready=%d link=%d speed=%"PRIu32"M duplex=%s",usb.vid,usb.pid,usb.configuration,usb.configurations,usb_net_driver_name(usb.driver),usb.stage,usb.ready,usb.link,usb.speed_mbps,usb.full_duplex?"full":"unknown/half");
    ESP_LOGI("status","bridge=%d learned=%d frames down/up=%"PRIu32"/%"PRIu32" drops=%"PRIu32"/%"PRIu32" malformed=%"PRIu32" other_clients=%"PRIu32,client.ready,client.learned,stats.frames_to_host,stats.frames_to_wifi,stats.drop_rx,stats.drop_tx,client.invalid_frames,client.other_clients);
    ESP_LOGI("status","USB RX blocks/frames/bad=%"PRIu32"/%"PRIu32"/%"PRIu32" TX=%"PRIu32" queue high/drop/stale=%"PRIu32"/%"PRIu32"/%"PRIu32" EP0 count/fail=%"PRIu32"/%"PRIu32" timeout=%"PRIu32" errors=%"PRIu32,usb.rx_transfers,usb.rx_frames,usb.rx_invalid,usb.tx_transfers,usb.queue_high_water,usb.queue_drops,usb.stale_drops,usb.control_requests,usb.control_failures,usb.timeouts,usb.errors);
    ESP_LOGI("status","DHCP eth/wifi=%"PRIu32"/%"PRIu32" ARP eth/wifi=%"PRIu32"/%"PRIu32" adoption tries/fails=%"PRIu32"/%"PRIu32" last WiFi TX err=%d",client.ethernet_dhcp,client.wifi_dhcp,client.ethernet_arp,client.wifi_arp,client.adoption_attempts,client.adoption_failures,client.last_wifi_tx_error);
    ESP_LOGI("status","heap free internal=%u min=%u PSRAM=%u largest internal=%u RX age=%"PRId64"ms TX age=%"PRId64"ms",(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),usb.last_rx_us?(now-usb.last_rx_us)/1000:-1,usb.last_tx_us?(now-usb.last_tx_us)/1000:-1);
    if(usb.last_error[0]) ESP_LOGW("status","Last USB error: %s",usb.last_error);
}
