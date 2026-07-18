#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

/*
 * NAT tether networking mode.
 *
 * In the default L2 bridge (bridge.c) the ESP holds no IP and the host adopts
 * the STA MAC. When storage/HID hosting is enabled we instead give the ESP its
 * own DHCP IP on the real Wi-Fi and route the USB host behind lwIP NAPT:
 *
 *   USB host --DHCP--> USB esp_netif (192.168.7.1/24) --NAPT--> Wi-Fi STA --> LAN
 *
 * The web UI / WebDAV / HID endpoints are then reachable both on the STA LAN IP
 * and on the USB private IP (192.168.7.1).
 */

#define NET_TETHER_USB_IP_A 192
#define NET_TETHER_USB_IP_B 168
#define NET_TETHER_USB_IP_C 7
#define NET_TETHER_USB_IP_D 1

/* Create the USB-side esp_netif (static IP + DHCP server) and bring it up.
 * Must be called after tinyusb_net_init(). */
esp_err_t net_tether_start(const uint8_t sta_mac[6]);

bool net_tether_active(void);

/* Feed a received NCM Ethernet frame into the USB netif (called from the NCM
 * receive callback while in NAT mode). */
void net_tether_input(void *buffer, uint16_t len);

/* Enable lwIP NAPT once the Wi-Fi STA has obtained an IP. Idempotent. */
void net_tether_enable_napt(void);

esp_netif_t *net_tether_usb_netif(void);
void net_tether_get_usb_ip(esp_netif_ip_info_t *out);
