/* ESP-IDF USB host adapter manager. The application owns one USB host install. */
#include "usb_ethernet.h"
#include "korvo_bridge.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#ifndef CONFIG_KORVO_USB_AUTO_CONFIGURATION
#define CONFIG_KORVO_USB_AUTO_CONFIGURATION 0
#endif

#define USB_BLOCK_MAX 16384
#define CONFIG_DESC_MAX 4096
#define TX_QUEUE_DEPTH 24
#define CONFIG_SCAN_MAX 16
static const char *TAG="usb_ethernet";
enum { CONTROL, RX0, RX1, NOTIFY, TX, SLOT_COUNT };
typedef struct { usb_transfer_t *transfer; bool busy,done; int64_t started; size_t frame_length; } slot_t;
typedef struct { uint32_t epoch; uint16_t length; int64_t queued_us; uint8_t data[BRIDGE_FRAME_MAX]; } packet_t;
typedef struct { uint16_t vid,pid,bcd; uint8_t configuration; uint16_t failed; } selection_t;
static slot_t s_slots[SLOT_COUNT];
static usb_host_client_handle_t s_client;
static usb_device_handle_t s_device;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_tx_queue;
static usb_ethernet_status_t s_status;
static usb_net_descriptor_t s_net,s_candidates[CONFIG_SCAN_MAX];
static usb_net_parameters_t s_parameters;
static selection_t s_selection;
static uint8_t s_probe[CONFIG_DESC_MAX];
static uint8_t s_addresses[16],s_address_count;
static uint32_t s_epoch;
static bool s_closing,s_flushed,s_control_claimed,s_data_claimed,s_power_cycle,s_running,s_seen_link_notification;
static int64_t s_last_link_poll;
static void process_input(void);
static void close_device(void);

static void detail(const char *message)
{
    xSemaphoreTake(s_lock,portMAX_DELAY);
    snprintf(s_status.detail,sizeof(s_status.detail),"%s",message);
    xSemaphoreGive(s_lock); ESP_LOGI(TAG,"%s",message);
}
static void stage(const char *name)
{
    xSemaphoreTake(s_lock,portMAX_DELAY); snprintf(s_status.stage,sizeof(s_status.stage),"%s",name); xSemaphoreGive(s_lock);
    ESP_LOGI(TAG,"Stage: %s",name);
}
static void link_state(bool up)
{
    xSemaphoreTake(s_lock,portMAX_DELAY);
    bool changed=s_status.link!=up; s_status.link=up;
    if(changed) {
        ++s_epoch;
        if(s_status.ready) snprintf(s_status.stage,sizeof(s_status.stage),"RUN / cable %s",up?"up":"down");
    }
    if(!up) s_status.speed_mbps=0;
    xSemaphoreGive(s_lock);
    if(changed) { ESP_LOGI(TAG,"Ethernet cable link %s",up?"UP":"DOWN"); korvo_bridge_ethernet_link(up); }
}
static void fail(const char *message)
{
    xSemaphoreTake(s_lock,portMAX_DELAY); ++s_status.errors; s_status.ready=false; ++s_epoch;
    snprintf(s_status.last_error,sizeof(s_status.last_error),"%s",message); xSemaphoreGive(s_lock);
    detail(message); stage("ERROR / reconnect USB"); link_state(false); s_running=false; s_closing=true;
}
static void reset_port(const char *reason)
{
    detail(reason); s_power_cycle=true; s_closing=true; s_running=false;
    xSemaphoreTake(s_lock,portMAX_DELAY); s_status.ready=false; ++s_epoch; xSemaphoreGive(s_lock);
    link_state(false); usb_host_lib_set_root_port_power(false);
}
static void transfer_error(usb_transfer_t *t,const char *operation)
{
    char error[128];
    snprintf(error,sizeof(error),"%s ep=%02x status=%d actual=%d requested=%d",operation,t->bEndpointAddress,t->status,t->actual_num_bytes,t->num_bytes);
    fail(error);
}
static void complete(usb_transfer_t *t)
{
    slot_t *slot=t->context; slot->busy=false; slot->done=true;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    s_status.last_endpoint=t->bEndpointAddress; s_status.last_transfer_status=t->status;
    if(slot==&s_slots[TX] && t->status==USB_TRANSFER_STATUS_COMPLETED) {
        ++s_status.tx_transfers; s_status.tx_bytes+=slot->frame_length; s_status.last_tx_us=esp_timer_get_time();
    }
    xSemaphoreGive(s_lock);
    if(slot==&s_slots[TX]) korvo_bridge_tx_result(!s_closing && t->status==USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes==t->num_bytes,slot->frame_length);
    if(!s_closing && s_running && slot>=&s_slots[RX0] && slot<=&s_slots[NOTIFY]) process_input();
}
static esp_err_t submit(int index,int bytes)
{
    slot_t *s=&s_slots[index]; s->transfer->device_handle=s_device; s->transfer->num_bytes=bytes; s->done=false;
    esp_err_t err=index==CONTROL?usb_host_transfer_submit_control(s_client,s->transfer):usb_host_transfer_submit(s->transfer);
    if(err==ESP_OK) { s->busy=true; s->started=esp_timer_get_time(); }
    else {
        xSemaphoreTake(s_lock,portMAX_DELAY); s_status.last_submit_error=err; xSemaphoreGive(s_lock);
        ESP_LOGE(TAG,"Submit slot=%d ep=%02x bytes=%d failed: %s",index,s->transfer->bEndpointAddress,bytes,esp_err_to_name(err));
    }
    return err;
}

/* Synchronous setup/polling runs only in the client worker, pumping that client's
 * events so unplug and data completions are serviced while EP0 is in flight.
 * No callback calls this function. Host 1.6 does not enforce timeout_ms itself. */
static int sync_control(void *ctx,uint8_t type,uint8_t request,uint16_t value,uint16_t index,void *data,uint16_t size)
{
    (void)ctx;
    if(s_closing || !s_device || size>CONFIG_DESC_MAX || s_slots[CONTROL].busy) return -1;
    usb_transfer_t *t=s_slots[CONTROL].transfer;
    usb_setup_packet_t *p=(usb_setup_packet_t *)t->data_buffer;
    *p=(usb_setup_packet_t){.bmRequestType=type,.bRequest=request,.wValue=value,.wIndex=index,.wLength=size};
    if(!(type&0x80) && size) memcpy(t->data_buffer+8,data,size);
    xSemaphoreTake(s_lock,portMAX_DELAY);
    ++s_status.control_requests; s_status.request_type=type; s_status.request=request;
    s_status.request_value=value; s_status.request_index=index; s_status.request_length=size;
    xSemaphoreGive(s_lock);
    if(submit(CONTROL,size+8)!=ESP_OK) return -1;
    while(s_slots[CONTROL].busy && !s_closing) {
        usb_host_client_handle_events(s_client,pdMS_TO_TICKS(10));
        if(esp_timer_get_time()-s_slots[CONTROL].started>2000000) {
            xSemaphoreTake(s_lock,portMAX_DELAY); ++s_status.timeouts; xSemaphoreGive(s_lock);
            reset_port("EP0 timeout; resetting USB host port"); break;
        }
    }
    s_slots[CONTROL].done=false;
    bool ok=!s_closing && !s_slots[CONTROL].busy && t->status==USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes>=8;
    int count=ok?t->actual_num_bytes-8:-1;
    if(count>size) { ok=false; count=-1; }
    if(!ok) {
        char error[128];
        snprintf(error,sizeof(error),"EP0 %02x/%02x value=%04x index=%04x len=%u status=%d",type,request,value,index,size,t->status);
        xSemaphoreTake(s_lock,portMAX_DELAY); ++s_status.control_failures;
        snprintf(s_status.last_error,sizeof(s_status.last_error),"%s",error); xSemaphoreGive(s_lock);
        ESP_LOGE(TAG,"%s",error);
    }
#if CONFIG_KORVO_USB_TRACE_CONTROL
    else ESP_LOGI(TAG,"EP0 %02x/%02x value=%04x index=%04x len=%u actual=%d",type,request,value,index,size,count);
#endif
    if(ok && (type&0x80) && count) memcpy(data,t->data_buffer+8,count);
    return ok ? ((type&0x80)?count:0) : -1;
}
static void io_sleep(void *ctx,unsigned ms)
{
    (void)ctx;
    int64_t until=esp_timer_get_time()+(int64_t)ms*1000;
    while(!s_closing && esp_timer_get_time()<until) usb_host_client_handle_events(s_client,pdMS_TO_TICKS(5));
}
static void client_event(const usb_host_client_event_msg_t *event,void *arg)
{
    (void)arg;
    if(event->event==USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGI(TAG,"USB device enumerated at address %u",event->new_dev.address);
        if(s_address_count<sizeof(s_addresses)) s_addresses[s_address_count++]=event->new_dev.address;
        else ESP_LOGW(TAG,"Device discovery queue full");
    } else if(event->event==USB_HOST_CLIENT_EVENT_DEV_REMOVED) {
        /* Failed/unsupported adapters have already been closed, so they do not
         * produce DEV_GONE on a later unplug. Clear failed-config history too. */
        xSemaphoreTake(s_lock,portMAX_DELAY);
        bool removed=!s_device && s_status.attached && s_status.address==event->dev_removed.address;
        if(removed) { s_status.attached=s_status.ready=false; memset(&s_selection,0,sizeof(s_selection)); }
        xSemaphoreGive(s_lock);
        if(removed) { stage("WAIT / USB-A HS"); detail("USB device removed; reconnect an Ethernet adapter"); }
    } else if(event->event==USB_HOST_CLIENT_EVENT_DEV_GONE && s_device==event->dev_gone.dev_hdl) {
        xSemaphoreTake(s_lock,portMAX_DELAY); s_status.attached=s_status.ready=false; ++s_epoch;
        if(!s_power_cycle) memset(&s_selection,0,sizeof(s_selection));
        xSemaphoreGive(s_lock); link_state(false);
        detail(s_power_cycle?"USB re-enumeration in progress":"Adapter unplugged; waiting for USB Ethernet");
        s_closing=true; s_running=false;
    }
}
static void describe_config(const uint8_t *d,size_t size,unsigned index)
{
    if(size<9) return;
    ESP_LOGI(TAG,"Configuration index=%u value=%u total=%u interfaces=%u power=%umA",index,d[5],(unsigned)size,d[4],d[8]*2);
    for(size_t p=0;p+2<=size && d[p]>=2 && d[p]<=size-p;p+=d[p]) {
        if(d[p+1]==4 && d[p]>=9) ESP_LOGI(TAG,"  interface=%u alt=%u class=%02x subclass=%02x protocol=%02x eps=%u",d[p+2],d[p+3],d[p+5],d[p+6],d[p+7],d[p+4]);
        else if(d[p+1]==5 && d[p]>=7) ESP_LOGI(TAG,"    endpoint=%02x kind=%u mps=%u interval=%u",d[p+2],d[p+3]&3,d[p+4]|d[p+5]<<8,d[p+6]);
    }
}
static bool choose_configuration(const usb_device_desc_t *dev,const usb_config_desc_t *active)
{
    memset(s_candidates,0,sizeof(s_candidates));
    usb_net_descriptor_t best={0};
    uint16_t failed=0;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    if(s_selection.vid==dev->idVendor && s_selection.pid==dev->idProduct && s_selection.bcd==dev->bcdDevice) failed=s_selection.failed;
    xSemaphoreGive(s_lock);
    unsigned configs=CONFIG_KORVO_USB_AUTO_CONFIGURATION?dev->bNumConfigurations:1;
    if(configs>CONFIG_SCAN_MAX) configs=CONFIG_SCAN_MAX;
    stage("Inspecting configurations");
    for(unsigned i=0;i<configs && !s_closing;++i) {
        const uint8_t *bytes=(const uint8_t *)active; size_t size=active->wTotalLength;
        if(CONFIG_KORVO_USB_AUTO_CONFIGURATION && i+1!=active->bConfigurationValue) {
            uint8_t header[9];
            int count=sync_control(NULL,0x80,6,0x0200|i,0,header,sizeof(header));
            if(count!=9 || header[0]<9 || header[1]!=2) continue;
            size=header[2]|header[3]<<8;
            if(size<9 || size>CONFIG_DESC_MAX) { ESP_LOGW(TAG,"Configuration %u exceeds descriptor limit (%u)",i+1,(unsigned)size); continue; }
            count=sync_control(NULL,0x80,6,0x0200|i,0,s_probe,size);
            if(count!=(int)size) continue;
            bytes=s_probe;
        }
        describe_config(bytes,size,i);
        usb_net_descriptor_t candidate;
        if(!usb_net_parse_config(bytes,size,dev->idVendor,dev->idProduct,&candidate)) continue;
        if(CONFIG_KORVO_USB_AUTO_CONFIGURATION && candidate.configuration!=i+1) {
            ESP_LOGW(TAG,"Host 1.6 requires config value=index+1; cannot select value %u at index %u",candidate.configuration,i); continue;
        }
        s_candidates[i]=candidate;
        ESP_LOGI(TAG,"  Candidate: %s config=%u",usb_net_driver_name(candidate.driver),candidate.configuration);
        if(failed&(1u<<i)) continue;
        if(usb_net_driver_priority(candidate.driver)>usb_net_driver_priority(best.driver) ||
           (candidate.driver==best.driver && candidate.configuration==active->bConfigurationValue)) best=candidate;
    }
    if(s_closing) return false;
    if(!best.driver) { fail("No usable USB Ethernet function; inspect descriptors and last EP0 error"); return false; }
    s_net=best;
    if(best.configuration!=active->bConfigurationValue) {
        xSemaphoreTake(s_lock,portMAX_DELAY);
        s_selection=(selection_t){dev->idVendor,dev->idProduct,dev->bcdDevice,best.configuration,failed};
        ++s_status.reconfigurations; xSemaphoreGive(s_lock);
        char message[128]; snprintf(message,sizeof(message),"Selecting config %u: %s; re-enumerating USB",best.configuration,usb_net_driver_name(best.driver));
        reset_port(message); return false;
    }
    return true;
}
static void setup_failed(const usb_device_desc_t *dev,const char *reason)
{
    if(s_closing) return;
    if(CONFIG_KORVO_USB_AUTO_CONFIGURATION && s_net.configuration<=CONFIG_SCAN_MAX) {
        uint16_t failed;
        xSemaphoreTake(s_lock,portMAX_DELAY);
        if(s_selection.vid!=dev->idVendor || s_selection.pid!=dev->idProduct || s_selection.bcd!=dev->bcdDevice)
            s_selection=(selection_t){dev->idVendor,dev->idProduct,dev->bcdDevice,0,0};
        s_selection.failed|=1u<<(s_net.configuration-1); failed=s_selection.failed;
        xSemaphoreGive(s_lock);
        usb_net_descriptor_t next={0};
        for(unsigned i=0;i<CONFIG_SCAN_MAX;++i) if(!(failed&(1u<<i)) && usb_net_driver_priority(s_candidates[i].driver)>usb_net_driver_priority(next.driver)) next=s_candidates[i];
        if(next.driver) {
            xSemaphoreTake(s_lock,portMAX_DELAY); s_selection.configuration=next.configuration; ++s_status.reconfigurations; xSemaphoreGive(s_lock);
            char message[128]; snprintf(message,sizeof(message),"%s setup failed; trying %s in config %u",usb_net_driver_name(s_net.driver),usb_net_driver_name(next.driver),next.configuration);
            reset_port(message); return;
        }
    }
    char error[128];
    xSemaphoreTake(s_lock,portMAX_DELAY);
    bool control_failed=s_status.control_failures && s_status.last_transfer_status!=USB_TRANSFER_STATUS_COMPLETED;
    snprintf(error,sizeof(error),"%s",control_failed?s_status.last_error:reason);
    xSemaphoreGive(s_lock);
    fail(error);
}
static void open_device(uint8_t address)
{
    if(usb_host_device_open(s_client,address,&s_device)!=ESP_OK) { detail("USB device disappeared before open"); return; }
    const usb_device_desc_t *dev; const usb_config_desc_t *config;
    if(usb_host_get_device_descriptor(s_device,&dev)!=ESP_OK || usb_host_get_active_config_descriptor(s_device,&config)!=ESP_OK) { fail("Cannot read USB descriptors"); return; }
    if(dev->bDeviceClass==9) { ESP_LOGI(TAG,"USB hub at %u; host library owns its interfaces",address); usb_host_device_close(s_client,s_device); s_device=NULL; return; }
    usb_device_info_t info={0}; usb_host_device_info(s_device,&info);
    xSemaphoreTake(s_lock,portMAX_DELAY);
    s_status.attached=true; s_status.ready=s_status.link=false; s_status.driver=USB_NET_NONE;
    s_status.vid=dev->idVendor; s_status.pid=dev->idProduct; s_status.bcd_device=dev->bcdDevice;
    s_status.address=address; s_status.configurations=dev->bNumConfigurations; s_status.configuration=config->bConfigurationValue;
    s_status.usb_speed=info.speed; memset(s_status.adapter_mac,0,6); s_status.product[0]=0;
    if(info.str_desc_product && info.str_desc_product->bLength>=2) {
        unsigned n=(info.str_desc_product->bLength-2)/2;
        if(n>=sizeof(s_status.product)) n=sizeof(s_status.product)-1;
        for(unsigned i=0;i<n;++i) { uint16_t c=info.str_desc_product->wData[i]; s_status.product[i]=(c>=32 && c<127)?c:'?'; }
        s_status.product[n]=0;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG,"USB %04x:%04x bcd=%04x addr=%u speed=%s configurations=%u active=%u product='%s'",dev->idVendor,dev->idProduct,dev->bcdDevice,address,info.speed==USB_SPEED_HIGH?"480M HS":info.speed==USB_SPEED_FULL?"12M FS":"low",dev->bNumConfigurations,config->bConfigurationValue,s_status.product);
    if(!choose_configuration(dev,config)) return;
    ecm_descriptor_t *ep=&s_net.endpoints;
    if(s_net.driver<USB_NET_AX88772) {
        if(usb_host_interface_claim(s_client,s_device,ep->control_interface,0)!=ESP_OK) { fail("Cannot claim USB network control interface"); return; }
        s_control_claimed=true;
    }
    if(usb_host_interface_claim(s_client,s_device,ep->data_interface,ep->data_alt)!=ESP_OK) { fail("Cannot claim USB network data interface"); return; }
    s_data_claimed=true; s_seen_link_notification=false;
    s_slots[RX0].transfer->bEndpointAddress=s_slots[RX1].transfer->bEndpointAddress=ep->bulk_in;
    s_slots[NOTIFY].transfer->bEndpointAddress=ep->notification;
    s_slots[TX].transfer->bEndpointAddress=ep->bulk_out; s_slots[TX].transfer->flags=USB_TRANSFER_FLAG_ZERO_PACK;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    s_status.driver=s_net.driver; s_status.configuration=s_net.configuration;
    s_status.control_interface=ep->control_interface; s_status.data_interface=ep->data_interface; s_status.data_alt=ep->data_alt;
    s_status.bulk_in=ep->bulk_in; s_status.bulk_out=ep->bulk_out; s_status.notification=ep->notification;
    s_status.in_mps=ep->in_mps; s_status.out_mps=ep->out_mps; xSemaphoreGive(s_lock);
    ESP_LOGI(TAG,"Driver %s: control=%u data=%u alt=%u in=%02x/%u out=%02x/%u notify=%02x",usb_net_driver_name(s_net.driver),ep->control_interface,ep->data_interface,ep->data_alt,ep->bulk_in,ep->in_mps,ep->bulk_out,ep->out_mps,ep->notification);
    stage("Initializing adapter");
    usb_net_io_t io={.control=sync_control,.sleep=io_sleep,.full_speed=info.speed!=USB_SPEED_HIGH};
    memset(&s_parameters,0,sizeof(s_parameters));
    uint8_t mac[6]={0}; uint16_t revision=0;
    bool initialized=s_net.driver>=USB_NET_AX88772?usb_net_native_init(s_net.driver,&io,mac,&revision):usb_net_class_init(&s_net,&io,&s_parameters,mac);
    xSemaphoreTake(s_lock,portMAX_DELAY); s_status.revision=revision; xSemaphoreGive(s_lock);
    ESP_LOGI(TAG,"Initialization %s revision=%04x result=%s",usb_net_driver_name(s_net.driver),revision,initialized?"OK":"FAILED");
    if(!initialized) { setup_failed(dev,io.error?io.error:"Adapter rejected initialization or returned invalid factory MAC"); return; }
    xSemaphoreTake(s_lock,portMAX_DELAY);
    memcpy(s_status.adapter_mac,mac,6); s_status.revision=revision; s_status.ready=true;
    s_status.ntb_in=s_parameters.in_max; s_status.ntb_out=s_parameters.out_max; xSemaphoreGive(s_lock);
    s_running=true; s_last_link_poll=0; stage("RUN / waiting for cable");
    ESP_LOGI(TAG,"Adapter MAC %02x:%02x:%02x:%02x:%02x:%02x revision=%04x NTB in/out=%"PRIu32"/%"PRIu32,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],revision,s_parameters.in_max,s_parameters.out_max);
    detail("USB Ethernet ready; waiting for cable / client traffic");
}
static bool deliver_frame(const uint8_t *data,size_t size,void *context)
{
    (void)context;
    xSemaphoreTake(s_lock,portMAX_DELAY); ++s_status.rx_frames; s_status.rx_bytes+=size; s_status.last_rx_us=esp_timer_get_time(); xSemaphoreGive(s_lock);
    if(!s_seen_link_notification && s_net.driver<USB_NET_AX88772 && s_net.driver!=USB_NET_RNDIS && bridge_frame_valid(data,size)) link_state(true);
    korvo_bridge_receive(data,size); return true;
}
static void process_input(void)
{
    for(int i=RX0;i<=NOTIFY;++i) {
        slot_t *slot=&s_slots[i]; usb_transfer_t *t=slot->transfer;
        if(!slot->done) continue;
        slot->done=false;
        if(t->status!=USB_TRANSFER_STATUS_COMPLETED) { transfer_error(t,"USB RX failed"); return; }
        size_t n=t->actual_num_bytes; const uint8_t *p=t->data_buffer;
        if(i==NOTIFY) {
            if(s_net.driver<=USB_NET_NCM && n>=8 && p[0]==0xa1 && p[4]==s_net.endpoints.control_interface && !p[5]) {
                if(p[1]==0 && !p[6] && !p[7]) { s_seen_link_notification=true; link_state((p[2]|p[3])!=0); }
                else if(p[1]==0x2a && n>=16 && p[6]==8 && !p[7]) {
                    uint32_t down=p[8]|(uint32_t)p[9]<<8|(uint32_t)p[10]<<16|(uint32_t)p[11]<<24;
                    xSemaphoreTake(s_lock,portMAX_DELAY); s_status.speed_mbps=down/1000000; xSemaphoreGive(s_lock);
                    ESP_LOGI(TAG,"CDC link speed down=%"PRIu32" bps",down);
                }
            }
        } else if(n) {
            xSemaphoreTake(s_lock,portMAX_DELAY); ++s_status.rx_transfers; xSemaphoreGive(s_lock);
            int frames=usb_net_decode(s_net.driver,p,n,deliver_frame,NULL);
            if(frames<0) {
                xSemaphoreTake(s_lock,portMAX_DELAY); uint32_t bad=++s_status.rx_invalid; xSemaphoreGive(s_lock);
                if(bad<=4 || !(bad&(bad-1))) ESP_LOGW(TAG,"Malformed %s RX block len=%u rejected (total=%"PRIu32")",usb_net_driver_name(s_net.driver),(unsigned)n,bad);
            }
        }
    }
}
static void receive_submit(void)
{
    for(int i=RX0;i<=NOTIFY;++i) {
        if(s_slots[i].busy || (i==NOTIFY && !s_net.endpoints.notification)) continue;
        unsigned mps=i==NOTIFY?s_net.endpoints.notification_mps:s_net.endpoints.in_mps;
        unsigned bytes=i==NOTIFY?mps:s_net.driver==USB_NET_ECM?BRIDGE_FRAME_MAX:
            s_net.driver==USB_NET_NCM?s_parameters.in_max:s_status.usb_speed==USB_SPEED_HIGH?USB_BLOCK_MAX:4096;
        bytes=(bytes+mps-1)/mps*mps;
        if(bytes>USB_BLOCK_MAX || submit(i,bytes)!=ESP_OK) { fail("Cannot submit receive transfer; inspect endpoint and buffer size"); return; }
    }
}
static void close_device(void)
{
    if(!s_flushed) {
        s_running=false; link_state(false);
        if(s_data_claimed) {
            usb_host_endpoint_halt(s_device,s_net.endpoints.bulk_in); usb_host_endpoint_flush(s_device,s_net.endpoints.bulk_in);
            usb_host_endpoint_halt(s_device,s_net.endpoints.bulk_out); usb_host_endpoint_flush(s_device,s_net.endpoints.bulk_out);
        }
        if(s_net.endpoints.notification && (s_control_claimed || s_data_claimed)) {
            usb_host_endpoint_halt(s_device,s_net.endpoints.notification); usb_host_endpoint_flush(s_device,s_net.endpoints.notification);
        }
        s_flushed=true;
    }
    for(int i=0;i<SLOT_COUNT;++i) if(s_slots[i].busy) return;
    if(s_data_claimed) usb_host_interface_release(s_client,s_device,s_net.endpoints.data_interface);
    if(s_control_claimed) usb_host_interface_release(s_client,s_device,s_net.endpoints.control_interface);
    usb_host_device_close(s_client,s_device); s_device=NULL;
    s_closing=s_flushed=s_control_claimed=s_data_claimed=false;
    for(int i=0;i<SLOT_COUNT;++i) s_slots[i].done=false;
    packet_t packet;
    while(xQueueReceive(s_tx_queue,&packet,0)==pdTRUE) korvo_bridge_tx_result(false,packet.length);
    if(s_power_cycle) { s_power_cycle=false; usb_host_lib_set_root_port_power(true); }
    ESP_LOGI(TAG,"USB resources drained and released; host remains ready for hotplug");
}
static void poll_link(void)
{
    if(s_net.driver<USB_NET_AX88772 && s_net.driver!=USB_NET_RNDIS) return;
    int64_t now=esp_timer_get_time(); if(now-s_last_link_poll<1000000) return; s_last_link_poll=now;
    usb_net_io_t io={.control=sync_control,.sleep=io_sleep,.full_speed=s_status.usb_speed!=USB_SPEED_HIGH};
    bool up=false,full=false; uint32_t speed=0;
    bool ok=s_net.driver==USB_NET_RNDIS?usb_net_rndis_link(&s_net,&io,&s_parameters,&up,&speed):usb_net_native_link(s_net.driver,&io,&up,&speed,&full);
    if(!ok && !s_closing) { fail("Ethernet link-status query failed; inspect last EP0 request"); return; }
    if(s_closing) return;
    s_seen_link_notification=true; link_state(up);
    xSemaphoreTake(s_lock,portMAX_DELAY); s_status.speed_mbps=speed; s_status.full_duplex=full; xSemaphoreGive(s_lock);
}
static void transmit_next(void)
{
    if(s_slots[TX].busy) return;
    packet_t packet;
    while(xQueueReceive(s_tx_queue,&packet,0)==pdTRUE) {
        xSemaphoreTake(s_lock,portMAX_DELAY);
        bool valid=s_status.link && s_status.ready && packet.epoch==s_epoch && esp_timer_get_time()-packet.queued_us<1000000;
        if(!valid) ++s_status.stale_drops;
        xSemaphoreGive(s_lock);
        if(!valid) { korvo_bridge_tx_result(false,packet.length); continue; }
        size_t n=usb_net_encode(s_net.driver,&s_parameters,s_net.endpoints.out_mps,packet.data,packet.length,s_slots[TX].transfer->data_buffer,USB_BLOCK_MAX);
        s_slots[TX].frame_length=packet.length;
        if(!n || submit(TX,n)!=ESP_OK) { korvo_bridge_tx_result(false,packet.length); fail("USB TX encoding or submit failed"); }
        return;
    }
}
static void usb_task(void *arg)
{
    (void)arg;
    for(;;) {
        usb_host_client_handle_events(s_client,pdMS_TO_TICKS(10));
        if(s_closing) { close_device(); continue; }
        if(!s_device && s_address_count) {
            uint8_t address=s_addresses[0]; memmove(s_addresses,s_addresses+1,--s_address_count); open_device(address);
        }
        if(s_closing || !s_device || !s_running) continue;
        if(s_slots[TX].busy && esp_timer_get_time()-s_slots[TX].started>500000) {
            xSemaphoreTake(s_lock,portMAX_DELAY); ++s_status.timeouts; ++s_status.errors; xSemaphoreGive(s_lock);
            reset_port("Bulk TX timeout; resetting USB host port"); continue;
        }
        if(s_slots[TX].done) {
            s_slots[TX].done=false;
            if(s_slots[TX].transfer->status!=USB_TRANSFER_STATUS_COMPLETED) { transfer_error(s_slots[TX].transfer,"USB TX failed"); continue; }
        }
        process_input(); if(s_closing) continue;
        poll_link(); if(s_closing) continue;
        receive_submit(); if(s_closing) continue;
        transmit_next();
    }
}
static void host_task(void *arg)
{
    (void)arg;
    for(;;) { uint32_t flags=0; usb_host_lib_handle_events(portMAX_DELAY,&flags); if(flags&USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all(); }
}
static bool select_configuration(const usb_device_desc_t *dev,uint8_t *configuration)
{
    *configuration=1;
    if(dev->bDeviceClass==9) return true;
    if(!CONFIG_KORVO_USB_AUTO_CONFIGURATION) *configuration=CONFIG_KORVO_USB_CONFIGURATION;
    else {
        xSemaphoreTake(s_lock,portMAX_DELAY);
        if(s_selection.vid==dev->idVendor && s_selection.pid==dev->idProduct && s_selection.bcd==dev->bcdDevice && s_selection.configuration) *configuration=s_selection.configuration;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG,"Enumeration %04x:%04x configurations=%u select=%u",dev->idVendor,dev->idProduct,dev->bNumConfigurations,*configuration);
    return true;
}
esp_err_t usb_ethernet_start(void)
{
    s_lock=xSemaphoreCreateMutex(); s_tx_queue=xQueueCreate(TX_QUEUE_DEPTH,sizeof(packet_t));
    if(!s_lock || !s_tx_queue) return ESP_ERR_NO_MEM;
    for(int i=0;i<SLOT_COUNT;++i) {
        esp_err_t err=usb_host_transfer_alloc(i==CONTROL?CONFIG_DESC_MAX+8:i==NOTIFY?1024:USB_BLOCK_MAX,0,&s_slots[i].transfer);
        if(err!=ESP_OK) return err;
        s_slots[i].transfer->callback=complete; s_slots[i].transfer->context=&s_slots[i];
    }
    const usb_host_config_t host={.intr_flags=ESP_INTR_FLAG_LOWMED,.enum_filter_cb=select_configuration,
#if CONFIG_KORVO_USB_FORCE_FULL_SPEED
        .fsls_only=true,
#endif
    };
    esp_err_t err=usb_host_install(&host); if(err!=ESP_OK) return err;
    const usb_host_client_config_t client={.max_num_event_msg=24,.flags={.notify_dev_removed=1},.async={.client_event_callback=client_event}};
    err=usb_host_client_register(&client,&s_client); if(err!=ESP_OK) return err;
    stage("WAIT / USB-A HS"); detail("Connect USB Ethernet: ECM / NCM / RNDIS / supported native chipset");
    if(xTaskCreate(host_task,"usb_host",4096,NULL,10,NULL)!=pdPASS || xTaskCreate(usb_task,"usb_ethernet",8192,NULL,7,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
esp_err_t usb_ethernet_send(const void *data,size_t length)
{
    if(!data || length<14 || length>BRIDGE_FRAME_MAX) return ESP_ERR_INVALID_SIZE;
    packet_t packet={.length=length,.queued_us=esp_timer_get_time()};
    xSemaphoreTake(s_lock,portMAX_DELAY); bool ready=s_status.ready && s_status.link; packet.epoch=s_epoch; xSemaphoreGive(s_lock);
    if(!ready) return ESP_ERR_INVALID_STATE;
    memcpy(packet.data,data,length);
    if(xQueueSend(s_tx_queue,&packet,0)!=pdTRUE) {
        xSemaphoreTake(s_lock,portMAX_DELAY); ++s_status.queue_drops; xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM;
    }
    unsigned depth=uxQueueMessagesWaiting(s_tx_queue);
    xSemaphoreTake(s_lock,portMAX_DELAY); if(depth>s_status.queue_high_water) s_status.queue_high_water=depth; xSemaphoreGive(s_lock);
    usb_host_client_unblock(s_client); return ESP_OK;
}
void usb_ethernet_status(usb_ethernet_status_t *out)
{
    xSemaphoreTake(s_lock,portMAX_DELAY); *out=s_status; xSemaphoreGive(s_lock);
}
void usb_ethernet_flush(void)
{
    if(!s_lock) return;
    xSemaphoreTake(s_lock,portMAX_DELAY); ++s_epoch; xSemaphoreGive(s_lock);
}
