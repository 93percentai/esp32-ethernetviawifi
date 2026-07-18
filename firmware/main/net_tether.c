#include "net_tether.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "tinyusb_net.h"

static const char *TAG = "net_tether";

typedef struct {
    esp_netif_driver_base_t base;
} usbnet_driver_t;

static esp_netif_t *s_usb_netif;
static usbnet_driver_t *s_driver;
static bool s_active;
static bool s_napt_on;

static esp_err_t usbnet_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    /* buff_free_arg = NULL: the lwIP pbuf is owned/freed by esp_netif after this
     * synchronous transmit returns; the NCM xmit callback copies it first. */
    esp_err_t err = tinyusb_net_send_sync(buffer, (uint16_t)len, NULL, pdMS_TO_TICKS(100));
    return (err == ESP_OK) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void usbnet_free_rx(void *h, void *buffer)
{
    (void)h;
    (void)buffer;
    /* RX frames are owned by TinyUSB (freed via tud_network_recv_renew). */
}

static esp_err_t usbnet_post_attach(esp_netif_t *esp_netif, void *args)
{
    usbnet_driver_t *drv = args;
    drv->base.netif = esp_netif;
    const esp_netif_driver_ifconfig_t ifcfg = {
        .handle = drv,
        .transmit = usbnet_transmit,
        .driver_free_rx_buffer = usbnet_free_rx,
    };
    return esp_netif_set_driver_config(esp_netif, &ifcfg);
}

esp_err_t net_tether_start(const uint8_t sta_mac[6])
{
    if (s_active) {
        return ESP_OK;
    }

    static esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, NET_TETHER_USB_IP_A, NET_TETHER_USB_IP_B, NET_TETHER_USB_IP_C, NET_TETHER_USB_IP_D);
    IP4_ADDR(&ip_info.gw, NET_TETHER_USB_IP_A, NET_TETHER_USB_IP_B, NET_TETHER_USB_IP_C, NET_TETHER_USB_IP_D);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    static const esp_netif_inherent_config_t base_cfg = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .ip_info = &ip_info,
        .if_key = "usb_lan",
        .if_desc = "usb",
        .route_prio = 10,
    };
    const esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    s_usb_netif = esp_netif_new(&cfg);
    ESP_RETURN_ON_FALSE(s_usb_netif, ESP_FAIL, TAG, "esp_netif_new failed");

    /* Give the USB netif a locally-administered MAC derived from the STA MAC. */
    uint8_t mac[6];
    memcpy(mac, sta_mac, 6);
    mac[0] |= 0x02;      /* locally administered */
    mac[5] ^= 0x01;      /* differ from STA */
    esp_netif_set_mac(s_usb_netif, mac);

    s_driver = calloc(1, sizeof(usbnet_driver_t));
    ESP_RETURN_ON_FALSE(s_driver, ESP_ERR_NO_MEM, TAG, "driver alloc failed");
    s_driver->base.post_attach = usbnet_post_attach;
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_usb_netif, s_driver), TAG, "attach failed");

    /* Bring the interface up and mark the (always-present) USB link connected. */
    esp_netif_action_start(s_usb_netif, NULL, 0, NULL);
    esp_netif_action_connected(s_usb_netif, NULL, 0, NULL);

    s_active = true;
    ESP_LOGI(TAG, "NAT tether USB netif up at %d.%d.%d.%d",
             NET_TETHER_USB_IP_A, NET_TETHER_USB_IP_B, NET_TETHER_USB_IP_C, NET_TETHER_USB_IP_D);
    return ESP_OK;
}

bool net_tether_active(void)
{
    return s_active;
}

void net_tether_input(void *buffer, uint16_t len)
{
    if (s_usb_netif) {
        esp_netif_receive(s_usb_netif, buffer, len, NULL);
    }
}

void net_tether_enable_napt(void)
{
    if (!s_active || s_napt_on || !s_usb_netif) {
        return;
    }
    esp_err_t err = esp_netif_napt_enable(s_usb_netif);
    if (err == ESP_OK) {
        s_napt_on = true;
        ESP_LOGI(TAG, "NAPT enabled (USB host -> Wi-Fi)");
    } else {
        ESP_LOGW(TAG, "esp_netif_napt_enable failed: %s (check LWIP_IPV4_NAPT)", esp_err_to_name(err));
    }
}

esp_netif_t *net_tether_usb_netif(void)
{
    return s_usb_netif;
}

void net_tether_get_usb_ip(esp_netif_ip_info_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_usb_netif) {
        esp_netif_get_ip_info(s_usb_netif, out);
    }
}
