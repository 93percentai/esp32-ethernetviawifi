#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb_net.h"

typedef struct {
    bool attached, ready, link, full_duplex;
    uint16_t vid, pid, bcd_device, revision;
    uint8_t adapter_mac[6], address, configuration, configurations, usb_speed;
    usb_net_driver_t driver;
    uint8_t control_interface, data_interface, data_alt, bulk_in, bulk_out, notification;
    uint16_t in_mps, out_mps;
    uint32_t errors, control_requests, control_failures, timeouts, reconfigurations;
    uint32_t rx_transfers, rx_frames, rx_invalid, tx_transfers, queue_drops, stale_drops, queue_high_water;
    uint32_t rx_bytes, tx_bytes, speed_mbps, ntb_in, ntb_out;
    int last_transfer_status, last_submit_error;
    uint8_t last_endpoint, request_type, request;
    uint16_t request_value, request_index, request_length;
    int64_t last_rx_us, last_tx_us;
    char stage[32], detail[128], last_error[128], product[64];
} usb_ethernet_status_t;

esp_err_t usb_ethernet_start(void);
/* Copies a complete Ethernet frame; bounded and never waits for USB I/O. */
esp_err_t usb_ethernet_send(const void *data, size_t length);
void usb_ethernet_flush(void);
void usb_ethernet_status(usb_ethernet_status_t *out);
