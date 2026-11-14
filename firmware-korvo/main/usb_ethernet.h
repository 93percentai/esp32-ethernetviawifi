#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool attached, ready, link;
    uint16_t vid, pid;
    uint8_t adapter_mac[6];
    uint32_t errors;
    char detail[96];
} usb_ethernet_status_t;

esp_err_t usb_ethernet_start(void);
/* Copies a complete Ethernet frame; bounded and never waits for USB I/O. */
esp_err_t usb_ethernet_send(const void *data, size_t length);
void usb_ethernet_flush(void);
void usb_ethernet_status(usb_ethernet_status_t *out);
