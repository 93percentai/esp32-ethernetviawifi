#pragma once
#include "platform.h"
typedef void *usb_host_client_handle_t;
typedef void *usb_device_handle_t;
typedef struct __attribute__((packed)) { uint8_t bmRequestType, bRequest; uint16_t wValue, wIndex, wLength; } usb_setup_packet_t;
typedef struct { uint8_t bDeviceClass; uint16_t idVendor, idProduct; } usb_device_desc_t;
typedef struct __attribute__((packed)) { uint8_t bLength, bDescriptorType; uint16_t wTotalLength; uint8_t bNumInterfaces, bConfigurationValue; } usb_config_desc_t;
typedef struct usb_transfer usb_transfer_t;
struct usb_transfer { uint8_t *data_buffer; int data_buffer_size, num_bytes, actual_num_bytes, status; usb_device_handle_t device_handle; uint8_t bEndpointAddress; unsigned flags; void *context; void (*callback)(usb_transfer_t *); };
enum {USB_TRANSFER_STATUS_COMPLETED, USB_TRANSFER_STATUS_CANCELED, USB_TRANSFER_STATUS_NO_DEVICE};
#define USB_TRANSFER_FLAG_ZERO_PACK 1
#define USB_HOST_CLIENT_EVENT_NEW_DEV 1
#define USB_HOST_CLIENT_EVENT_DEV_GONE 2
#define USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS 1
typedef struct { int event; union { struct { uint8_t address; } new_dev; struct { usb_device_handle_t dev_hdl; } dev_gone; }; } usb_host_client_event_msg_t;
typedef struct { int intr_flags; bool (*enum_filter_cb)(const usb_device_desc_t *, uint8_t *); } usb_host_config_t;
typedef struct { int max_num_event_msg; struct { void (*client_event_callback)(const usb_host_client_event_msg_t *, void *); } async; } usb_host_client_config_t;
esp_err_t usb_host_transfer_alloc(int, int, usb_transfer_t **);
esp_err_t usb_host_transfer_submit(usb_transfer_t *);
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t, usb_transfer_t *);
esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t *);
esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t);
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t, const usb_device_desc_t **);
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t, const usb_config_desc_t **);
esp_err_t usb_host_interface_claim(usb_host_client_handle_t, usb_device_handle_t, uint8_t, uint8_t);
esp_err_t usb_host_interface_release(usb_host_client_handle_t, usb_device_handle_t, uint8_t);
esp_err_t usb_host_endpoint_halt(usb_device_handle_t, uint8_t);
esp_err_t usb_host_endpoint_flush(usb_device_handle_t, uint8_t);
esp_err_t usb_host_lib_set_root_port_power(bool);
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t, unsigned);
esp_err_t usb_host_lib_handle_events(unsigned, uint32_t *);
esp_err_t usb_host_client_unblock(usb_host_client_handle_t);
esp_err_t usb_host_device_free_all(void);
esp_err_t usb_host_install(const usb_host_config_t *);
esp_err_t usb_host_client_register(const usb_host_client_config_t *, usb_host_client_handle_t *);
