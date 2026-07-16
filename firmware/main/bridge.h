#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint64_t bytes_to_host;   /* Wi-Fi → USB  (host download) */
    uint64_t bytes_to_wifi;   /* USB → Wi-Fi  (host upload) */
    uint32_t frames_to_host;
    uint32_t frames_to_wifi;
    uint32_t drop_tx;         /* dropped host→wifi (not associated) */
    uint32_t drop_rx;         /* dropped wifi→host (USB busy) */
    uint32_t drop_refl;       /* AP-reflected own frames */
    float rate_to_host_bps;   /* smoothed bytes/sec */
    float rate_to_wifi_bps;
} bridge_stats_t;

esp_err_t bridge_init(const uint8_t sta_mac[6]);
void bridge_set_wifi_up(bool up);
void bridge_get_stats(bridge_stats_t *out);
void bridge_reset_stats(void);

/* Passive-readable helpers for LCD / console */
void bridge_format_bytes(uint64_t bytes, char *out, size_t out_len);
void bridge_format_rate(float bps, char *out, size_t out_len);
