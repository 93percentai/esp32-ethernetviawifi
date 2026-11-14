#include <stdio.h>
#include "../firmware-korvo/main/usb_ethernet.c"

static uint8_t config[] = {
    9,2,0,0,2,2,0,0x80,50,
    9,4,3,0,1,2,6,0,0,
    5,0x24,6,3,7,
    13,0x24,0x0f,4,0,0,0,0,0xea,5,0,0,0,
    7,5,0x83,3,16,0,8,
    9,4,7,0,0,0x0a,0,0,0,
    9,4,7,2,2,0x0a,0,0,0,
    7,5,0x85,2,0,2,0,
    7,5,6,2,0,2,0,
};
static int64_t clock_us = 1000;
static unsigned received, closed, released, sent_success, sent_fail;
static uint8_t receive_order[10];
static unsigned claimed;
static bool bridge_link;
int64_t esp_timer_get_time(void) { return clock_us; }
void korvo_bridge_ethernet_link(bool up) { bridge_link = up; }
void korvo_bridge_receive(const uint8_t *f, size_t n) { assert(n == 60); receive_order[received++] = f[14]; }
void korvo_bridge_tx_result(bool success, size_t n) { assert(n >= 14); if(success) ++sent_success; else ++sent_fail; }

esp_err_t usb_host_transfer_alloc(int n, int iso, usb_transfer_t **out) {
    assert(!iso); *out = calloc(1, sizeof(**out)); assert(*out);
    (*out)->data_buffer = calloc(1, n); (*out)->data_buffer_size = n; return ESP_OK;
}
esp_err_t usb_host_transfer_submit(usb_transfer_t *t) { assert(t->num_bytes <= t->data_buffer_size); return ESP_OK; }
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t h, usb_transfer_t *t) { (void)h; return usb_host_transfer_submit(t); }
esp_err_t usb_host_device_open(usb_host_client_handle_t h, uint8_t a, usb_device_handle_t *out) { (void)h; assert(a == 1); *out = config; return ESP_OK; }
esp_err_t usb_host_device_close(usb_host_client_handle_t h, usb_device_handle_t d) {
    (void)h; assert(d == config);
    for (int i=0;i<SLOT_COUNT;++i) assert(!s_slots[i].busy);
    ++closed; return ESP_OK;
}
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t d, const usb_device_desc_t **out) {
    (void)d; static const usb_device_desc_t dev = {.idVendor=0x1234,.idProduct=0x5678}; *out=&dev; return ESP_OK;
}
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t d, const usb_config_desc_t **out) { *out=d; return ESP_OK; }
esp_err_t usb_host_interface_claim(usb_host_client_handle_t h, usb_device_handle_t d, uint8_t n, uint8_t a) {
    (void)h; (void)d; assert((n == 3 && a == 0) || (n == 7 && a == 2)); ++claimed; return ESP_OK;
}
esp_err_t usb_host_interface_release(usb_host_client_handle_t h, usb_device_handle_t d, uint8_t n) { (void)h; (void)d; assert(n == 3 || n == 7); ++released; return ESP_OK; }
esp_err_t usb_host_endpoint_halt(usb_device_handle_t d, uint8_t ep) { (void)d; assert(ep); return ESP_OK; }
esp_err_t usb_host_endpoint_flush(usb_device_handle_t d, uint8_t ep) { (void)d; assert(ep); return ESP_OK; }
esp_err_t usb_host_lib_set_root_port_power(bool on) { (void)on; return ESP_OK; }
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t h, unsigned t) { (void)h; (void)t; return ESP_OK; }
esp_err_t usb_host_lib_handle_events(unsigned t, uint32_t *f) { (void)t; *f=0; return ESP_OK; }
esp_err_t usb_host_client_unblock(usb_host_client_handle_t h) { (void)h; return ESP_OK; }
esp_err_t usb_host_device_free_all(void) { return ESP_OK; }
esp_err_t usb_host_install(const usb_host_config_t *c) { assert(c->enum_filter_cb); return ESP_OK; }
esp_err_t usb_host_client_register(const usb_host_client_config_t *c, usb_host_client_handle_t *out) { assert(c->async.client_event_callback); *out=config; return ESP_OK; }

static void finish_control(const uint8_t *data, size_t n) {
    usb_transfer_t *t = s_slots[CONTROL].transfer; assert(s_slots[CONTROL].busy);
    if (n) memcpy(t->data_buffer + 8, data, n);
    t->actual_num_bytes = 8+n; t->status = USB_TRANSFER_STATUS_COMPLETED; t->callback(t); setup_step();
}
static usb_setup_packet_t request(void) { return *(usb_setup_packet_t *)s_slots[CONTROL].transfer->data_buffer; }
static void configure(void) {
    open_device(1); assert(s_stage == 1 && request().wValue == 0x300);
    const uint8_t lang[] = {4,3,9,4}; finish_control(lang,4);
    assert(s_stage == 2 && request().wValue == 0x304 && request().wIndex == 0x409);
    uint8_t mac[26] = {26,3}; const char *text="021122334455";
    for (unsigned i=0;i<12;++i) mac[2+2*i]=text[i];
    finish_control(mac,26);
    assert(request().bRequest == 11 && request().wValue == 2 && request().wIndex == 7);
    finish_control(NULL,0);
    assert(request().bRequest == 0x43 && request().wValue == 0xf && request().wIndex == 3);
    finish_control(NULL,0);
    assert(s_stage == 0 && s_status.ready && !s_status.link);
}
static void notify(bool up) {
    usb_transfer_t *t=s_slots[NOTIFY].transfer;
    const uint8_t bytes[] = {0xa1,0,up,0,3,0,0,0};
    memcpy(t->data_buffer, bytes, sizeof(bytes)); t->actual_num_bytes=8;
    t->status=USB_TRANSFER_STATUS_COMPLETED; complete(t);
}
static void receive_frame(int index, uint8_t sequence) {
    usb_transfer_t *t=s_slots[index].transfer;
    memset(t->data_buffer,0,60); t->data_buffer[6]=2; t->data_buffer[12]=8; t->data_buffer[14]=sequence;
    t->actual_num_bytes=60; t->status=USB_TRANSFER_STATUS_COMPLETED; complete(t);
}
int main(void) {
    config[2]=sizeof(config); assert(usb_ethernet_start()==ESP_OK);
    configure(); assert(claimed==2);
    receive_submit(); assert(s_slots[RX0].busy && s_slots[RX1].busy);
    assert(s_slots[TX].transfer->flags == USB_TRANSFER_FLAG_ZERO_PACK);
    assert(s_slots[RX0].transfer->num_bytes==1536);
    notify(true); assert(bridge_link && s_status.link);
    /* Complete slot 1 before slot 0: callback order must survive. */
    receive_frame(RX1,1); receive_submit(); receive_frame(RX0,2);
    assert(received==2 && receive_order[0]==1 && receive_order[1]==2);
    uint8_t frame[60]={0}; frame[6]=2;
    for(unsigned i=0;i<24;++i) assert(usb_ethernet_send(frame,sizeof(frame))==ESP_OK);
    assert(usb_ethernet_send(frame,sizeof(frame))==ESP_ERR_NO_MEM);
    packet_t packet; assert(xQueueReceive(s_tx_queue,&packet,0));
    assert(packet.epoch==s_epoch); usb_ethernet_flush(); assert(packet.epoch!=s_epoch);
    notify(false); assert(!bridge_link && !s_status.link);
    receive_frame(RX0,3); assert(!s_status.link); /* Stale RX cannot undo an explicit cable-down. */
    assert(usb_ethernet_send(frame,sizeof(frame))==ESP_ERR_INVALID_STATE);
    notify(true); receive_submit();
    const usb_host_client_event_msg_t gone={.event=USB_HOST_CLIENT_EVENT_DEV_GONE,.dev_gone={.dev_hdl=s_device}};
    client_event(&gone,NULL); close_device();
    assert(s_device && !closed && !released); /* Pending URBs prevent premature handle release. */
    const usb_host_client_event_msg_t next={.event=USB_HOST_CLIENT_EVENT_NEW_DEV,.new_dev={.address=1}};
    client_event(&next,NULL); assert(s_new_address==1); /* Fast replug is retained during cleanup. */
    for(int i=0;i<SLOT_COUNT;++i) if(s_slots[i].busy) {
        s_slots[i].transfer->status=USB_TRANSFER_STATUS_NO_DEVICE; complete(s_slots[i].transfer);
    }
    close_device(); assert(!s_device && closed==1 && released==2 && !s_tx_queue->count);
    configure(); assert(s_status.ready); /* Same persistent transfer pool supports unplug/replug. */
    fail("test shutdown"); close_device(); assert(!s_device && closed==2);
    open_device(1); assert(s_slots[CONTROL].busy);
    client_event(&gone,NULL); close_device(); assert(s_device && closed==2);
    s_slots[CONTROL].transfer->status=USB_TRANSFER_STATUS_NO_DEVICE;
    complete(s_slots[CONTROL].transfer);
    close_device(); assert(!s_device && closed==3); /* Unplug during EP0 setup is also drained. */
    for(int i=0;i<SLOT_COUNT;++i) { free(s_slots[i].transfer->data_buffer); free(s_slots[i].transfer); }
    free(s_lock); free(s_tx_queue->bytes); free(s_tx_queue);
    puts("USB setup, promiscuous filter, frame order, queue bounds and unplug/replug passed");
}
