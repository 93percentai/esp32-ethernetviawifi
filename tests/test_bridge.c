#include <stdio.h>
#include "../firmware-korvo/main/bridge.c"
static unsigned wifi_sent, wifi_freed, usb_sent, flushed;
static esp_err_t usb_result, wifi_result;
static esp_err_t (*registered)(void *, uint16_t, void *);
int64_t esp_timer_get_time(void) { return 1000000; }
esp_err_t esp_wifi_internal_tx(int interface, void *data, size_t size) {
    assert(interface==WIFI_IF_STA && data && size==60); ++wifi_sent; return wifi_result;
}
void esp_wifi_internal_free_rx_buffer(void *buffer) { assert(buffer); ++wifi_freed; }
esp_err_t esp_wifi_internal_reg_rxcb(int interface, esp_err_t (*cb)(void *,uint16_t,void *)) { assert(interface==WIFI_IF_STA); registered=cb; return ESP_OK; }
esp_err_t usb_ethernet_send(const void *data, size_t size) { assert(data && size==60); ++usb_sent; return usb_result; }
void usb_ethernet_flush(void) { ++flushed; }
bool wifi_mgr_is_connected(void) { return true; }
esp_err_t wifi_mgr_adopt_mac(const uint8_t mac[6]) { (void)mac; return ESP_OK; }
bool provisioning_is_active(void) { return false; }
esp_err_t provisioning_start(bridge_config_t *cfg) { (void)cfg; return ESP_OK; }
int main(void) {
    uint8_t frame[60]={0}; frame[6]=2; frame[11]=1; frame[12]=8;
    assert(bridge_init(NULL)==ESP_OK);
    korvo_bridge_ethernet_link(true);
    korvo_bridge_receive(frame,sizeof(frame));
    assert(s_client.learned && !s_client.ready && wifi_sent==0);
    bridge_set_wifi_up(true); assert(registered);
    korvo_bridge_receive(frame,sizeof(frame)); assert(wifi_sent==0); /* Wi-Fi still has board MAC. */
    s_mac_matches=true; bridge_set_wifi_up(true);
    korvo_bridge_receive(frame,sizeof(frame)); assert(wifi_sent==1 && s_stats.frames_to_wifi==1);
    frame[11]=2; korvo_bridge_receive(frame,sizeof(frame));
    assert(wifi_sent==1 && s_client.other_clients==1); /* A second source cannot hijack the bridge. */
    frame[11]=1;
    wifi_result=ESP_FAIL; korvo_bridge_receive(frame,sizeof(frame));
    assert(wifi_sent==2 && s_stats.frames_to_wifi==1);
    /* Traffic addressed to client; remote source differs from adopted MAC. */
    memcpy(frame,s_client.client_mac,6); frame[11]=8;
    assert(registered(frame,sizeof(frame),frame)==ESP_OK);
    assert(wifi_freed==1 && usb_sent==1);
    usb_result=ESP_ERR_NO_MEM;
    assert(registered(frame,sizeof(frame),frame)==ESP_OK);
    assert(wifi_freed==2 && usb_sent==2);
    frame[11]=1; /* Reflected own broadcast is discarded and freed. */
    registered(frame,sizeof(frame),frame); assert(wifi_freed==3 && usb_sent==2 && s_stats.drop_refl==1);
    frame[11]=8; frame[5]=0x99; /* Wrong unicast destination. */
    registered(frame,sizeof(frame),frame); assert(wifi_freed==4 && usb_sent==2);
    frame[0]=0xff; registered(frame,sizeof(frame),frame); assert(wifi_freed==5 && usb_sent==3);
    registered(frame,3,frame); assert(wifi_freed==6 && usb_sent==3);
    bridge_set_wifi_up(false); assert(flushed==1 && !s_client.ready);
    registered(frame,sizeof(frame),frame); assert(wifi_freed==7 && usb_sent==3);
    korvo_bridge_ethernet_link(false); assert(!s_client.learned && !s_client.ready);
    korvo_bridge_ethernet_link(true);
    frame[6]=2; frame[11]=2; korvo_bridge_receive(frame,sizeof(frame));
    assert(s_client.client_mac[5]==2 && !s_client.ready);
    puts("Single-client learning, MAC gating, loop filtering and Wi-Fi buffer ownership passed");
}
