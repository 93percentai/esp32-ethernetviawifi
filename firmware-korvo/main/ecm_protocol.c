#include "ecm_protocol.h"
#include <string.h>

static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }

bool ecm_parse_config(const uint8_t *d, size_t n, ecm_descriptor_t *out)
{
    if (!d || !out || n < 9 || d[0] < 9 || d[1] != 2) return false;
    size_t end = le16(d + 2);
    if (end < d[0] || end > n) return false;
    /* Reject a malformed chain before examining any interface. */
    for (size_t p = 0; p < end;) {
        if (end - p < 2 || d[p] < 2 || d[p] > end - p) return false;
        p += d[p];
    }
    for (size_t c = d[0]; c < end; c += d[c]) {
        if (d[c + 1] != 4 || d[c] < 9 || d[c + 3] != 0 ||
            d[c + 5] != 2 || d[c + 6] != 6 || d[c + 7] != 0) continue;
        ecm_descriptor_t e = {.control_interface = d[c + 2]};
        bool has_union = false, has_ethernet = false;
        for (size_t p = c + d[c]; p < end && d[p + 1] != 4; p += d[p]) {
            if (d[p + 1] == 0x24 && d[p] >= 3) {
                if (d[p + 2] == 6 && d[p] == 5 && d[p + 3] == e.control_interface) {
                    e.data_interface = d[p + 4]; has_union = true;
                } else if (d[p + 2] == 0x0f && d[p] >= 13) {
                    e.mac_string = d[p + 3]; e.max_segment = le16(d + p + 8);
                    has_ethernet = true;
                }
            } else if (d[p + 1] == 5 && d[p] >= 7 && (d[p + 2] & 0x80) &&
                       (d[p + 2] & 0x0f) && !(d[p + 2] & 0x70) &&
                       (d[p + 3] & 3) == 3) {
                e.notification = d[p + 2]; e.notification_mps = le16(d + p + 4) & 0x7ff;
            }
        }
        if (!has_union || !has_ethernet || !e.mac_string ||
            e.data_interface == e.control_interface || e.max_segment < BRIDGE_FRAME_MAX ||
            !e.notification || e.notification_mps < 8 || e.notification_mps > 1024) continue;
        for (size_t p = d[0]; p < end; p += d[p]) {
            if (d[p + 1] != 4 || d[p] < 9 || d[p + 2] != e.data_interface ||
                d[p + 5] != 0x0a || d[p + 4] != 2) continue;
            e.bulk_in = e.bulk_out = 0;
            e.data_alt = d[p + 3];
            for (size_t q = p + d[p]; q < end && d[q + 1] != 4; q += d[q]) {
                if (d[q + 1] != 5 || d[q] < 7 || (d[q + 3] & 3) != 2) continue;
                uint8_t ep = d[q + 2];
                uint16_t mps = le16(d + q + 4);
                if (!(ep & 0x0f) || (ep & 0x70) || !mps || mps > 512 || (mps & (mps - 1))) continue;
                if (ep & 0x80) { e.bulk_in = ep; e.in_mps = mps; }
                else { e.bulk_out = ep; e.out_mps = mps; }
            }
            if (e.bulk_in && e.bulk_out) { *out = e; return true; }
        }
    }
    return false;
}

static int hex(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool ecm_parse_mac(const uint8_t *d, size_t n, uint8_t mac[6])
{
    if (!d || !mac || n < 26 || d[0] != 26 || d[1] != 3) return false;
    uint8_t parsed[6];
    for (int i = 0; i < 6; ++i) {
        int a = hex(d[2 + i * 4]), b = hex(d[4 + i * 4]);
        if (a < 0 || b < 0 || d[3 + i * 4] || d[5 + i * 4]) return false;
        parsed[i] = (a << 4) | b;
    }
    static const uint8_t zero[6];
    if ((parsed[0] & 1) || !memcmp(parsed, zero, 6)) return false;
    memcpy(mac, parsed, 6);
    return true;
}

bool bridge_frame_valid(const uint8_t *f, size_t n)
{
    static const uint8_t zero[6];
    /* Standard untagged Ethernet only; MTU 1500. Do not learn multicast sources. */
    return f && n >= 14 && n <= BRIDGE_FRAME_MAX && !(f[6] & 1) &&
           memcmp(f + 6, zero, 6) && !(f[12] == 0x81 && f[13] == 0) &&
           !(f[12] == 0x88 && f[13] == 0xa8);
}

bool bridge_frame_can_learn(const uint8_t *f, size_t n)
{
    if (!bridge_frame_valid(f, n)) return false;
    unsigned type = (f[12] << 8) | f[13];
    return type == 0x0800 || type == 0x0806 || type == 0x86dd;
}
