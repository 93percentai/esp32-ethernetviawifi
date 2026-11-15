#include <stdio.h>
#include "../firmware-korvo/main/usb_ethernet.c"

static uint8_t configs[3][96];
static size_t config_size;
static uint8_t active_config=1,config_count=1;
static usb_transfer_t *pending_control;
static int64_t clock_us=1000;
static unsigned received,closed,released,claimed,sent_success,sent_fail,filters,resets;
static uint8_t receive_order[32];
static bool bridge_link,stall_control;
static uint8_t reject_filter_configuration;
static int host_devices=1;
static unsigned power_on_attempts;
static esp_err_t power_on_error,power_off_error;
static usb_device_desc_t device_desc={.idVendor=0x1234,.idProduct=0x5678,.bcdDevice=0x0100};
static const uint8_t fixture[]={
    9,2,0,0,2,1,0,0x80,50, 9,4,3,0,1,2,6,0,0,
    5,0x24,6,3,7, 13,0x24,0xf,4,0,0,0,0,0xea,5,0,0,0,
    6,0x24,0x1a,0,1,1, 7,5,0x83,3,16,0,8, 9,4,7,0,0,10,0,0,0,
    9,4,7,2,2,10,0,0,0, 7,5,0x85,2,0,2,0, 7,5,6,2,0,2,0,
};
int64_t esp_timer_get_time(void) { return clock_us; }
void korvo_bridge_ethernet_link(bool up) { bridge_link=up; }
void korvo_bridge_receive(const uint8_t *f,size_t n) { assert(n==60 && received<sizeof(receive_order)); receive_order[received++]=f[14]; }
void korvo_bridge_tx_result(bool success,size_t n) { assert(n>=14); if(success) ++sent_success; else ++sent_fail; }
esp_err_t usb_host_transfer_alloc(int n,int iso,usb_transfer_t **out) {
    assert(!iso); *out=calloc(1,sizeof(**out)); assert(*out); (*out)->data_buffer=calloc(1,n); (*out)->data_buffer_size=n; return ESP_OK;
}
esp_err_t usb_host_transfer_submit(usb_transfer_t *t) { assert(t->num_bytes<=t->data_buffer_size); return ESP_OK; }
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t h,usb_transfer_t *t) { (void)h; pending_control=t; return usb_host_transfer_submit(t); }
esp_err_t usb_host_device_open(usb_host_client_handle_t h,uint8_t a,usb_device_handle_t *out) { (void)h; assert(a==1); *out=configs; return ESP_OK; }
esp_err_t usb_host_device_close(usb_host_client_handle_t h,usb_device_handle_t d) {
    (void)h; assert(d==configs);
    for(int i=0;i<SLOT_COUNT;++i) assert(!s_slots[i].busy);
    ++closed; return ESP_OK;
}
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t d,const usb_device_desc_t **out) {
    (void)d; device_desc.bNumConfigurations=config_count; *out=&device_desc; return ESP_OK;
}
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t d,const usb_config_desc_t **out) { (void)d; *out=(void *)configs[active_config-1]; return ESP_OK; }
esp_err_t usb_host_device_info(usb_device_handle_t d,usb_device_info_t *out) { (void)d; out->speed=USB_SPEED_HIGH; out->str_desc_product=NULL; return ESP_OK; }
esp_err_t usb_host_interface_claim(usb_host_client_handle_t h,usb_device_handle_t d,uint8_t n,uint8_t a) {
    (void)h; (void)d; assert((n==s_net.endpoints.control_interface && a==0) || (n==s_net.endpoints.data_interface && a==s_net.endpoints.data_alt)); ++claimed; return ESP_OK;
}
esp_err_t usb_host_interface_release(usb_host_client_handle_t h,usb_device_handle_t d,uint8_t n) { (void)h; (void)d; assert(n==s_net.endpoints.control_interface || n==s_net.endpoints.data_interface); ++released; return ESP_OK; }
esp_err_t usb_host_endpoint_halt(usb_device_handle_t d,uint8_t ep) { (void)d; assert(ep); return ESP_OK; }
esp_err_t usb_host_endpoint_flush(usb_device_handle_t d,uint8_t ep) { (void)d; assert(ep); return ESP_OK; }
esp_err_t usb_host_lib_set_root_port_power(bool on) {
    if(!on) { ++resets; return power_off_error; }
    ++power_on_attempts;
    /* A downstream NIC close is insufficient: its hub must also be free. */
    assert(!host_devices && !s_device);
    if(power_on_error) return power_on_error;
    const usb_device_desc_t *d; usb_host_get_device_descriptor(configs,&d); select_configuration(d,&active_config);
    host_devices=1;
    return ESP_OK;
}
esp_err_t usb_host_lib_info(usb_host_lib_info_t *info) {
    *info=(usb_host_lib_info_t){.num_devices=host_devices,.num_clients=1}; return ESP_OK;
}
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t h,unsigned ticks) {
    (void)h; clock_us+=(int64_t)ticks*1000;
    if(pending_control && !stall_control) {
        usb_transfer_t *t=pending_control; pending_control=NULL;
        usb_setup_packet_t request=*(usb_setup_packet_t *)t->data_buffer;
        uint8_t *data=t->data_buffer+8; size_t n=request.wLength;
        if(request.bmRequestType==0x80 && request.bRequest==6) {
            if((request.wValue>>8)==2) { unsigned index=request.wValue&255; assert(index<config_count); memcpy(data,configs[index],n); }
            else if(request.wValue==0x0300) { const uint8_t lang[]={4,3,9,4}; assert(n==4); memcpy(data,lang,4); }
            else { const char *mac="021122334455"; assert(n==26); memset(data,0,n); data[0]=26; data[1]=3; for(unsigned i=0;i<12;++i) data[2+i*2]=mac[i]; }
        } else if(request.bmRequestType==0xa1 && request.bRequest==0x80) {
            assert(n==28); memset(data,0,n);data[0]=28;data[2]=1;data[5]=0x40;data[17]=0x40;data[20]=4;data[24]=4;
        } else {
            if(request.bRequest==0x43 && reject_filter_configuration==active_config) {
                t->actual_num_bytes=8;t->status=3;t->callback(t);return ESP_OK;
            }
            assert(request.bRequest==11 || request.bRequest==0x43 || request.bRequest==0x86);
            if(request.bRequest==0x43) { assert(request.wValue==15 && request.wIndex==s_net.endpoints.control_interface); ++filters; }
        }
        t->actual_num_bytes=n+8; t->status=USB_TRANSFER_STATUS_COMPLETED; t->callback(t);
    }
    return ESP_OK;
}
esp_err_t usb_host_lib_handle_events(unsigned t,uint32_t *f) { (void)t; *f=0; return ESP_OK; }
esp_err_t usb_host_client_unblock(usb_host_client_handle_t h) { (void)h; return ESP_OK; }
esp_err_t usb_host_device_free_all(void) { return ESP_OK; }
esp_err_t usb_host_install(const usb_host_config_t *c) { assert(c->enum_filter_cb); return ESP_OK; }
esp_err_t usb_host_client_register(const usb_host_client_config_t *c,usb_host_client_handle_t *out) { assert(c->async.client_event_callback); *out=configs; return ESP_OK; }
static void notify(bool up) {
    usb_transfer_t *t=s_slots[NOTIFY].transfer; const uint8_t bytes[]={0xa1,0,up,0,3,0,0,0};
    memcpy(t->data_buffer,bytes,8); t->actual_num_bytes=8; t->status=USB_TRANSFER_STATUS_COMPLETED; complete(t);
}
static void receive_frame(int index,uint8_t seq) {
    usb_transfer_t *t=s_slots[index].transfer; memset(t->data_buffer,0,60); t->data_buffer[6]=2; t->data_buffer[12]=8; t->data_buffer[14]=seq;
    t->actual_num_bytes=60; t->status=USB_TRANSFER_STATUS_COMPLETED; complete(t);
}
static void drain(void) {
    if(s_device) close_device();
    for(int i=0;i<SLOT_COUNT;++i) if(s_slots[i].busy) { s_slots[i].transfer->status=USB_TRANSFER_STATUS_NO_DEVICE; complete(s_slots[i].transfer); }
    if(s_device) close_device();
}
static void finish_reset(void) {
    assert(s_power_cycle && !s_device);
    unsigned attempts=power_on_attempts;
    /* Simulate the hub remaining alive after NIC close and a delayed removal
     * notification (the ordering seen in the hardware capture). */
    host_devices=1;
    recover_port(); clock_us+=300000; recover_port();
    assert(s_power_cycle && power_on_attempts==attempts);
    const usb_host_client_event_msg_t removed={.event=USB_HOST_CLIENT_EVENT_DEV_REMOVED,.dev_removed={.address=1}};
    uint8_t selected=s_selection.configuration; uint16_t failed=s_selection.failed;
    client_event(&removed,NULL);
    assert(s_selection.configuration==selected && s_selection.failed==failed);
    const usb_host_client_event_msg_t stale={.event=USB_HOST_CLIENT_EVENT_NEW_DEV,.new_dev={.address=1}};
    client_event(&stale,NULL); assert(!s_address_count);
    host_devices=0; recover_port(); clock_us+=300000; recover_port();
    assert(power_on_attempts==attempts); /* zero devices alone is not ALL_FREE */
    host_events(USB_HOST_LIB_EVENT_FLAGS_ALL_FREE); recover_port();
    clock_us+=199000; recover_port(); assert(power_on_attempts==attempts);
    power_on_error=ESP_ERR_INVALID_STATE;
    clock_us+=1000; recover_port(); assert(power_on_attempts==attempts+1 && s_power_cycle);
    clock_us+=99000; recover_port(); assert(power_on_attempts==attempts+1);
    power_on_error=ESP_OK;
    clock_us+=1000; recover_port(); assert(!s_power_cycle && power_on_attempts==attempts+2);
    /* A final old notification cannot erase the cached selection after ON. */
    client_event(&removed,NULL); assert(s_selection.configuration==selected && s_selection.failed==failed);
}
int main(void) {
    config_size=sizeof(fixture); assert(config_size<sizeof(configs[0]));
    for(unsigned i=0;i<3;++i) { memcpy(configs[i],fixture,config_size); configs[i][2]=config_size; configs[i][5]=i+1; }
    assert(usb_ethernet_start()==ESP_OK); open_device(1);
    assert(s_status.ready && s_net.driver==USB_NET_ECM && claimed==2 && filters==1);
    receive_submit(); assert(s_slots[RX0].busy && s_slots[RX1].busy);
    assert(s_slots[TX].transfer->flags==USB_TRANSFER_FLAG_ZERO_PACK && s_slots[RX0].transfer->num_bytes==1536);
    notify(true); assert(bridge_link);
    receive_frame(RX1,1); receive_submit(); receive_frame(RX0,2);
    assert(received==2 && receive_order[0]==1 && receive_order[1]==2);
    uint8_t frame[60]={0}; frame[6]=2;
    for(unsigned i=0;i<24;++i) assert(usb_ethernet_send(frame,sizeof(frame))==ESP_OK);
    assert(usb_ethernet_send(frame,sizeof(frame))==ESP_ERR_NO_MEM && s_status.queue_drops==1 && s_status.queue_high_water==24);
    usb_ethernet_flush(); transmit_next(); assert(!s_tx_queue->count && s_status.stale_drops==24);
    assert(usb_ethernet_send(frame,sizeof(frame))==ESP_OK); transmit_next(); assert(s_slots[TX].busy);
    s_slots[TX].transfer->actual_num_bytes=60; s_slots[TX].transfer->status=USB_TRANSFER_STATUS_COMPLETED; complete(s_slots[TX].transfer);
    assert(sent_success==1 && s_status.tx_transfers==1);
    notify(false); receive_frame(RX0,3); assert(!s_status.link);
    notify(true); receive_submit();
    const usb_host_client_event_msg_t gone={.event=USB_HOST_CLIENT_EVENT_DEV_GONE,.dev_gone={.dev_hdl=s_device}};
    client_event(&gone,NULL); close_device(); assert(s_device && !closed && !released);
    const usb_host_client_event_msg_t next={.event=USB_HOST_CLIENT_EVENT_NEW_DEV,.new_dev={.address=1}};
    client_event(&next,NULL); assert(s_address_count==1);
    drain(); assert(!s_device && closed==1 && released==2);
#if CONFIG_KORVO_USB_AUTO_CONFIGURATION
    /* Generic discovery: config 1 vendor-only, config 2 NCM, config 3 ECM.
     * It must inspect descriptors first, choose ECM, then re-enumerate once. */
    config_count=3; configs[0][14]=0xff; configs[1][15]=0x0d;
    open_device(1); assert(s_closing && !s_status.ready && resets==1 && s_selection.configuration==3);
    client_event(&gone,NULL); drain(); assert(active_config==1); finish_reset(); assert(active_config==3);
    open_device(1); assert(s_status.ready && s_net.driver==USB_NET_ECM && s_status.configuration==3);
    assert(s_status.reconfigurations==1 && filters==2);
    fail("test shutdown"); drain(); assert(!s_device);
    /* A class initialization failure must select the next viable configuration
     * exactly once and keep the failed EP0 request for diagnostics. */
    reject_filter_configuration=3;
    open_device(1);assert(s_closing && s_selection.configuration==2 && s_status.control_failures==1);
    assert(strstr(s_status.last_error,"EP0"));
    client_event(&gone,NULL);drain();finish_reset();assert(active_config==2);
    open_device(1);assert(s_status.ready && s_net.driver==USB_NET_NCM && s_parameters.in_max==16384);
    fail("test fallback shutdown");drain();reject_filter_configuration=0;
    assert(s_status.attached && s_selection.failed);
    const usb_host_client_event_msg_t removed={.event=USB_HOST_CLIENT_EVENT_DEV_REMOVED,.dev_removed={.address=1}};
    client_event(&removed,NULL);assert(!s_status.attached && !s_selection.failed);
#endif
    /* A stuck EP0 has a finite deadline, retains the pending transfer until
     * cancellation, and resets the host port rather than freeing a live URB. */
    config_count=1; active_config=1; configs[0][14]=2; stall_control=true;
    open_device(1); assert(s_closing && s_status.timeouts==1 && s_slots[CONTROL].busy);
    pending_control=NULL; drain(); assert(!s_device); finish_reset(); stall_control=false;
    /* Exact RTL8153 identity from the UART report selects ECM on the first
     * enumeration. Other revisions/counts and manual mode keep their defaults. */
    device_desc=(usb_device_desc_t){.idVendor=0x0bda,.idProduct=0x8153,.bcdDevice=0x3100,.bNumConfigurations=2};
    uint8_t selected=0; select_configuration(&device_desc,&selected);
    assert(selected==(CONFIG_KORVO_USB_AUTO_CONFIGURATION?2:CONFIG_KORVO_USB_CONFIGURATION));
    device_desc.bcdDevice=0x3000; select_configuration(&device_desc,&selected);
    assert(selected==(CONFIG_KORVO_USB_AUTO_CONFIGURATION?1:CONFIG_KORVO_USB_CONFIGURATION));
    device_desc.bcdDevice=0x3100; device_desc.bNumConfigurations=1; select_configuration(&device_desc,&selected);
    assert(selected==(CONFIG_KORVO_USB_AUTO_CONFIGURATION?1:CONFIG_KORVO_USB_CONFIGURATION));
#if CONFIG_KORVO_USB_AUTO_CONFIGURATION
    device_desc.bNumConfigurations=config_count=2;
    select_configuration(&device_desc,&active_config);
    const uint8_t realtek_ecm[]={
        9,2,80,0,2,2,0,0x80,50, 9,4,0,0,1,2,6,0,0,
        5,0x24,6,0,1, 13,0x24,0xf,4,0,0,0,0,0xea,5,0,0,0,
        7,5,0x83,3,16,0,8, 9,4,1,0,0,10,0,0,0,
        9,4,1,1,2,10,0,0,0, 7,5,0x81,2,0,2,0, 7,5,2,2,0,2,0,
        5,0x24,0,0x10,1,
    };
    assert(sizeof(realtek_ecm)==80);
    memcpy(configs[1],realtek_ecm,sizeof(realtek_ecm));
    configs[0][14]=0xff;
    unsigned before=resets;
    open_device(1);
    assert(s_status.ready && s_status.vid==0x0bda && s_status.pid==0x8153);
    assert(s_status.configuration==2 && s_net.driver==USB_NET_ECM && resets==before);
    assert(s_status.bulk_in==0x81 && s_status.bulk_out==2 && s_status.data_alt==1);
    receive_submit();
    reset_port("test hub drain timeout"); drain();
    host_devices=1; clock_us+=5000001; recover_port();
    assert(!s_power_cycle && s_status.reset_failures==1 && s_status.last_host_error==ESP_ERR_TIMEOUT);
    assert(strstr(s_status.stage,"ERROR") && strstr(s_status.detail,"reboot board"));
#else
    /* Also exercise host error reporting in the manual-configuration build. */
    power_off_error=ESP_FAIL; reset_port("test power-off error");
    assert(!s_power_cycle && s_status.reset_failures==1 && !s_closing);
#endif
    for(int i=0;i<SLOT_COUNT;++i) { free(s_slots[i].transfer->data_buffer); free(s_slots[i].transfer); }
    free(s_lock); free(s_tx_queue->bytes); free(s_tx_queue);
    puts("USB configuration discovery, RTL8153 first enumeration, hub reset lifecycle, hotplug, queues and timeout passed");
}
