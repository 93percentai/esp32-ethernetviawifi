#pragma once
#include "ecm_protocol.h"

typedef enum {
    USB_NET_NONE, USB_NET_ECM, USB_NET_NCM, USB_NET_RNDIS,
    USB_NET_AX88772, USB_NET_RTL8152, USB_NET_DM9601, USB_NET_LAN95XX, USB_NET_AX88179,
} usb_net_driver_t;

typedef struct {
    usb_net_driver_t driver;
    ecm_descriptor_t endpoints;
    uint8_t configuration, ncm_capabilities;
} usb_net_descriptor_t;

typedef struct {
    uint32_t in_max, out_max;
    uint16_t out_divisor, out_remainder, out_alignment, sequence;
    uint8_t ncm_capabilities;
    uint32_t request_id;
} usb_net_parameters_t;

typedef bool (*usb_net_frame_cb_t)(const uint8_t *, size_t, void *);
/* Transport returns actual IN data length or zero for successful OUT; negative on failure. */
typedef struct {
    int (*control)(void *, uint8_t, uint8_t, uint16_t, uint16_t, void *, uint16_t);
    void (*sleep)(void *, unsigned);
    void *context;
    bool failed, full_speed;
    const char *error;
} usb_net_io_t;

const char *usb_net_driver_name(usb_net_driver_t driver);
int usb_net_driver_priority(usb_net_driver_t driver);
usb_net_driver_t usb_net_native_match(uint16_t vid, uint16_t pid);
bool usb_net_parse_config(const uint8_t *data, size_t size, uint16_t vid, uint16_t pid, usb_net_descriptor_t *out);
/* Decode validates the entire USB block before publishing its first frame. */
int usb_net_decode(usb_net_driver_t driver, const uint8_t *data, size_t size, usb_net_frame_cb_t callback, void *context);
size_t usb_net_encode(usb_net_driver_t driver, usb_net_parameters_t *parameters, uint16_t mps,
                      const void *frame, size_t size, uint8_t *out, size_t capacity);
bool usb_net_ncm_parameters(const uint8_t *data, size_t size, usb_net_parameters_t *out);
int usb_net_control(usb_net_io_t *, uint8_t, uint8_t, uint16_t, uint16_t, void *, uint16_t);
void usb_net_sleep(usb_net_io_t *, unsigned);
bool usb_net_native_init(usb_net_driver_t, usb_net_io_t *, uint8_t mac[6], uint16_t *revision);
/* Link polling updates speed/duplex and configures media-dependent MAC registers. */
bool usb_net_native_link(usb_net_driver_t, usb_net_io_t *, bool *up, uint32_t *speed_mbps, bool *full_duplex);
bool usb_net_rtl8152_init(usb_net_io_t *, uint8_t mac[6], uint16_t *revision);
bool usb_net_rtl8152_link(usb_net_io_t *, bool *up, uint32_t *speed, bool *full);
bool usb_net_class_init(const usb_net_descriptor_t *, usb_net_io_t *, usb_net_parameters_t *, uint8_t mac[6]);
bool usb_net_rndis_link(const usb_net_descriptor_t *, usb_net_io_t *, usb_net_parameters_t *, bool *up, uint32_t *speed);
