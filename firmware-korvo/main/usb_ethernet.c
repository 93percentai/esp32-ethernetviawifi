/* CDC ECM host, using ESP-IDF USB host (no TinyUSB device stack).
 * Board host initialization follows esp32_s31_korvo_1 1.0.1 bsp_usb.c.
 */
#include "usb_ethernet.h"
#include "ecm_protocol.h"
#include "korvo_bridge.h"
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

static const char *TAG = "usb_ethernet";
enum { CONTROL, RX0, RX1, NOTIFY, TX, SLOT_COUNT };
typedef struct { usb_transfer_t *transfer; bool busy, done; int64_t started; } slot_t;
typedef struct { uint32_t epoch; uint16_t length; uint8_t data[BRIDGE_FRAME_MAX]; } packet_t;
static slot_t s_slots[SLOT_COUNT];
static usb_host_client_handle_t s_client;
static usb_device_handle_t s_device;
static ecm_descriptor_t s_ecm;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_tx_queue;
static usb_ethernet_status_t s_status;
static uint32_t s_epoch;
static uint8_t s_new_address;
static bool s_closing, s_flushed, s_control_claimed, s_data_claimed, s_power_cycle;
static int s_stage;
static uint16_t s_language;
static bool s_seen_link_notification;
static void process_input(void);

static void detail(const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status.detail, sizeof(s_status.detail), "%s", message);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", message);
}

static void link_state(bool up)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_status.link != up;
    s_status.link = up;
    if (changed) ++s_epoch;
    xSemaphoreGive(s_lock);
    if (changed) {
        ESP_LOGI(TAG, "Ethernet cable link %s", up ? "up" : "down");
        korvo_bridge_ethernet_link(up);
    }
}

static void fail(const char *message)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ++s_status.errors;
    s_status.ready = false;
    ++s_epoch;
    xSemaphoreGive(s_lock);
    detail(message);
    link_state(false);
    s_closing = true;
}

static void complete(usb_transfer_t *t)
{
    slot_t *slot = t->context;
    slot->busy = false;
    slot->done = true;
    if (slot == &s_slots[TX]) {
        korvo_bridge_tx_result(!s_closing && t->status == USB_TRANSFER_STATUS_COMPLETED,
                               t->num_bytes);
    }
    /* Consume RX immediately in callback order, even when two URBs finish in
     * the same event batch. Scanning slots later can reorder Ethernet frames. */
    if (!s_closing && !s_stage && slot >= &s_slots[RX0] && slot <= &s_slots[NOTIFY]) process_input();
}

static esp_err_t submit(int index, int bytes)
{
    slot_t *s = &s_slots[index];
    s->transfer->device_handle = s_device;
    s->transfer->num_bytes = bytes;
    s->done = false;
    esp_err_t err = index == CONTROL ? usb_host_transfer_submit_control(s_client, s->transfer) :
                                      usb_host_transfer_submit(s->transfer);
    if (err == ESP_OK) { s->busy = true; s->started = esp_timer_get_time(); }
    return err;
}

static bool control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t size)
{
    usb_setup_packet_t *p = (usb_setup_packet_t *)s_slots[CONTROL].transfer->data_buffer;
    *p = (usb_setup_packet_t){.bmRequestType = type, .bRequest = request,
        .wValue = value, .wIndex = index, .wLength = size};
    if (submit(CONTROL, sizeof(*p) + size) != ESP_OK) {
        fail("USB control submit failed; reconnect adapter");
        return false;
    }
    return true;
}

static void client_event(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (!s_device || s_closing) s_new_address = event->new_dev.address;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE && s_device == event->dev_gone.dev_hdl) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.attached = s_status.ready = false;
        ++s_epoch;
        xSemaphoreGive(s_lock);
        link_state(false);
        detail("Adapter unplugged; waiting for USB-A Ethernet");
        s_closing = true;
    }
}

static void open_device(uint8_t address)
{
    if (usb_host_device_open(s_client, address, &s_device) != ESP_OK) return;
    const usb_device_desc_t *dev;
    const usb_config_desc_t *config;
    if (usb_host_get_device_descriptor(s_device, &dev) != ESP_OK ||
        usb_host_get_active_config_descriptor(s_device, &config) != ESP_OK) {
        fail("Cannot read USB descriptors"); return;
    }
    /* Hubs belong to the host library. Leave them available for downstream devices. */
    if (dev->bDeviceClass == 9) { usb_host_device_close(s_client, s_device); s_device = NULL; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.attached = true;
    s_status.vid = dev->idVendor; s_status.pid = dev->idProduct;
    memset(s_status.adapter_mac, 0, 6);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "USB %04x:%04x config=%u", dev->idVendor, dev->idProduct, config->bConfigurationValue);
    if (!ecm_parse_config((const uint8_t *)config, config->wTotalLength, &s_ecm)) {
        fail("No supported CDC ECM interface; check USB configuration/chipset"); return;
    }
    if (usb_host_interface_claim(s_client, s_device, s_ecm.control_interface, 0) != ESP_OK) {
        fail("Cannot claim ECM control interface"); return;
    }
    s_control_claimed = true;
    if (usb_host_interface_claim(s_client, s_device, s_ecm.data_interface, s_ecm.data_alt) != ESP_OK) {
        fail("Cannot claim ECM data interface"); return;
    }
    s_data_claimed = true;
    s_seen_link_notification = false;
    s_slots[RX0].transfer->bEndpointAddress = s_ecm.bulk_in;
    s_slots[RX1].transfer->bEndpointAddress = s_ecm.bulk_in;
    s_slots[NOTIFY].transfer->bEndpointAddress = s_ecm.notification;
    s_slots[TX].transfer->bEndpointAddress = s_ecm.bulk_out;
    s_slots[TX].transfer->flags = USB_TRANSFER_FLAG_ZERO_PACK;
    detail("Configuring USB CDC ECM adapter");
    s_stage = 1;
    control(0x80, 6, 0x0300, 0, 4); /* Get supported string language, not an assumed locale. */
}

static void setup_step(void)
{
    slot_t *s = &s_slots[CONTROL];
    if (!s_stage || s->busy || !s->done || s_closing) return;
    s->done = false;
    usb_transfer_t *t = s->transfer;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) { fail("ECM control request failed; reconnect adapter"); return; }
    const uint8_t *data = t->data_buffer + 8;
    size_t count = t->actual_num_bytes >= 8 ? t->actual_num_bytes - 8 : 0;
    switch (s_stage) {
    case 1:
        if (count < 4 || data[0] < 4 || data[1] != 3) { fail("Invalid USB string language descriptor"); return; }
        s_language = data[2] | ((uint16_t)data[3] << 8);
        s_stage = 2; control(0x80, 6, 0x0300 | s_ecm.mac_string, s_language, 26); break;
    case 2: {
        uint8_t mac[6];
        if (!ecm_parse_mac(data, count, mac)) { fail("Invalid ECM adapter MAC string"); return; }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_status.adapter_mac, mac, 6);
        xSemaphoreGive(s_lock);
        s_stage = 3; control(0x01, 11, s_ecm.data_alt, s_ecm.data_interface, 0); break;
    }
    case 3:
        /* Promiscuous reception is essential: Ethernet destination is the client,
         * whose MAC differs from the USB adapter's factory MAC. Fail if rejected. */
        s_stage = 4; control(0x21, 0x43, 0x000f, s_ecm.control_interface, 0); break;
    case 4:
        s_stage = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.ready = true;
        xSemaphoreGive(s_lock);
        detail("CDC ECM ready; connect one Ethernet client");
        break;
    }
}

static void process_input(void)
{
    for (int i = RX0; i <= NOTIFY; ++i) {
        slot_t *s = &s_slots[i];
        usb_transfer_t *t = s->transfer;
        if (!s->done) continue;
        s->done = false;
        if (t->status != USB_TRANSFER_STATUS_COMPLETED) { fail("USB receive error; reconnect adapter"); return; }
        size_t n = t->actual_num_bytes;
        const uint8_t *p = t->data_buffer;
        if (i == NOTIFY) {
            if (n >= 8 && p[0] == 0xa1 && p[4] == s_ecm.control_interface && !p[5] &&
                p[1] == 0 && !p[6] && !p[7]) {
                s_seen_link_notification = true;
                link_state((p[2] | p[3]) != 0);
            }
        } else if (n) {
            /* A completed ECM bulk transfer is exactly one frame, never a byte stream. */
            if (!s_seen_link_notification && bridge_frame_valid(p, n)) link_state(true);
            korvo_bridge_receive(p, n);
        }
    }
}

static void receive_submit(void)
{
    for (int i = RX0; i <= NOTIFY; ++i) {
        if (s_slots[i].busy) continue;
        int mps = i == NOTIFY ? s_ecm.notification_mps : s_ecm.in_mps;
        int capacity = i == NOTIFY ? mps :
                                   ((BRIDGE_FRAME_MAX + mps - 1) / mps) * mps;
        if (submit(i, capacity) != ESP_OK) { fail("USB receive submit failed"); return; }
    }
}

static void close_device(void)
{
    if (!s_flushed) {
        link_state(false);
        if (s_data_claimed) {
            usb_host_endpoint_halt(s_device, s_ecm.bulk_in);
            usb_host_endpoint_flush(s_device, s_ecm.bulk_in);
            usb_host_endpoint_halt(s_device, s_ecm.bulk_out);
            usb_host_endpoint_flush(s_device, s_ecm.bulk_out);
        }
        if (s_control_claimed) {
            usb_host_endpoint_halt(s_device, s_ecm.notification);
            usb_host_endpoint_flush(s_device, s_ecm.notification);
        }
        s_flushed = true;
    }
    for (int i = 0; i < SLOT_COUNT; ++i) if (s_slots[i].busy) return;
    /* Every callback has returned before releasing claimed interfaces and handle. */
    if (s_data_claimed) usb_host_interface_release(s_client, s_device, s_ecm.data_interface);
    if (s_control_claimed) usb_host_interface_release(s_client, s_device, s_ecm.control_interface);
    usb_host_device_close(s_client, s_device);
    s_device = NULL;
    s_stage = 0;
    s_closing = s_flushed = s_control_claimed = s_data_claimed = false;
    for (int i = 0; i < SLOT_COUNT; ++i) s_slots[i].done = false;
    xQueueReset(s_tx_queue);
    if (s_power_cycle) { s_power_cycle = false; usb_host_lib_set_root_port_power(true); }
}

static void usb_task(void *arg)
{
    (void)arg;
    packet_t packet;
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
        if (s_new_address && !s_device) {
            uint8_t address = s_new_address; s_new_address = 0;
            open_device(address);
        }
        if (s_closing) { close_device(); continue; }
        if (!s_device) continue;
        /* Host 1.6.0 does not implement transfer timeout_ms. Enforce a deadline
         * ourselves. Pending EP0 requests require bus removal before teardown. */
        int64_t now = esp_timer_get_time();
        if ((s_slots[CONTROL].busy && now - s_slots[CONTROL].started > 2000000) ||
            (s_slots[TX].busy && now - s_slots[TX].started > 500000)) {
            fail("USB adapter stalled; resetting host port");
            s_power_cycle = true;
            usb_host_lib_set_root_port_power(false);
            continue;
        }
        setup_step();
        if (s_closing || s_stage) continue;
        process_input();
        if (s_closing) continue;
        receive_submit();
        if (s_closing) continue;
        if (!s_slots[TX].busy && xQueueReceive(s_tx_queue, &packet, 0) == pdTRUE) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool valid = s_status.link && s_status.ready && packet.epoch == s_epoch;
            xSemaphoreGive(s_lock);
            if (!valid) { korvo_bridge_tx_result(false, packet.length); continue; }
            memcpy(s_slots[TX].transfer->data_buffer, packet.data, packet.length);
            if (submit(TX, packet.length) != ESP_OK) {
                korvo_bridge_tx_result(false, packet.length);
                fail("USB transmit submit failed");
            }
        }
    }
}

static void host_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static bool select_configuration(const usb_device_desc_t *dev, uint8_t *configuration)
{
    /* Do not force the Ethernet configuration selection on a USB hub. */
    *configuration = dev->bDeviceClass == 9 ? 1 : CONFIG_KORVO_USB_CONFIGURATION;
    return true;
}

esp_err_t usb_ethernet_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_tx_queue = xQueueCreate(24, sizeof(packet_t));
    if (!s_lock || !s_tx_queue) return ESP_ERR_NO_MEM;
    for (int i = 0; i < SLOT_COUNT; ++i) {
        esp_err_t err = usb_host_transfer_alloc(i == CONTROL ? 64 : 2048, 0, &s_slots[i].transfer);
        if (err != ESP_OK) return err;
        s_slots[i].transfer->callback = complete;
        s_slots[i].transfer->context = &s_slots[i];
    }
    const usb_host_config_t host = {.intr_flags = ESP_INTR_FLAG_LOWMED,
        .enum_filter_cb = select_configuration}; /* Default peripheral is S31 high speed. */
    esp_err_t err = usb_host_install(&host);
    if (err != ESP_OK) return err;
    const usb_host_client_config_t client = {.max_num_event_msg = 12,
        .async = {.client_event_callback = client_event}};
    err = usb_host_client_register(&client, &s_client);
    if (err != ESP_OK) return err;
    detail("Connect a CDC ECM Ethernet adapter to USB-A HS");
    if (xTaskCreate(host_task, "usb_host", 4096, NULL, 10, NULL) != pdPASS ||
        xTaskCreate(usb_task, "usb_ethernet", 6144, NULL, 7, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t usb_ethernet_send(const void *data, size_t length)
{
    if (!data || length < 14 || length > BRIDGE_FRAME_MAX) return ESP_ERR_INVALID_SIZE;
    packet_t packet = {.length = length};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_status.ready && s_status.link;
    packet.epoch = s_epoch;
    xSemaphoreGive(s_lock);
    if (!ready) return ESP_ERR_INVALID_STATE;
    memcpy(packet.data, data, length);
    if (xQueueSend(s_tx_queue, &packet, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    usb_host_client_unblock(s_client);
    return ESP_OK;
}

void usb_ethernet_status(usb_ethernet_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

void usb_ethernet_flush(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY); ++s_epoch; xSemaphoreGive(s_lock);
}
