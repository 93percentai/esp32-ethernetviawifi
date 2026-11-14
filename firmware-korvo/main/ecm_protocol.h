#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BRIDGE_FRAME_MAX 1514

typedef struct {
    uint8_t control_interface, data_interface, data_alt;
    uint8_t bulk_in, bulk_out, notification, mac_string;
    uint16_t in_mps, out_mps, notification_mps, max_segment;
} ecm_descriptor_t;

/* Parse only within the supplied configuration, binding CDC Union interfaces. */
bool ecm_parse_config(const uint8_t *data, size_t size, ecm_descriptor_t *out);
bool ecm_parse_mac(const uint8_t *descriptor, size_t size, uint8_t mac[6]);
bool bridge_frame_valid(const uint8_t *frame, size_t size);
bool bridge_frame_can_learn(const uint8_t *frame, size_t size);
