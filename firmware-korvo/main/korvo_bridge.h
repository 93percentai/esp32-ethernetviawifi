#pragma once
#include "../../firmware/main/bridge.h"
#include "config_store.h"

typedef struct {
    bool learned, ready;
    uint8_t client_mac[6];
    uint32_t other_clients, invalid_frames;
    uint32_t ethernet_dhcp, wifi_dhcp, ethernet_arp, wifi_arp;
    uint32_t adoption_attempts, adoption_failures;
    int last_wifi_tx_error;
} korvo_bridge_status_t;

esp_err_t korvo_bridge_start(bridge_config_t *cfg);
void korvo_bridge_status(korvo_bridge_status_t *out);
void korvo_bridge_receive(const uint8_t *frame, size_t length);
void korvo_bridge_ethernet_link(bool up);
void korvo_bridge_tx_result(bool success, size_t length);
void korvo_request_setup(void);
