#include "ecm_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Composite descriptor: ECM control interface 3, data interface 7 (not 4),
 * alt 2 (not 1), with unrelated vendor interfaces on both sides. */
static const uint8_t configuration[] = {
    9,2, 0,0, 3,1,0,0x80,50,
    9,4, 0,0,0,0xff,0,0,0,
    9,4, 3,0,1,2,6,0,0,
    5,0x24,0,0x20,1,
    5,0x24,6,3,7,
    13,0x24,0x0f,4,0,0,0,0,0xea,5,0,0,0,
    7,5,0x83,3,16,0,8,
    9,4, 7,0,0,0x0a,0,0,0,
    9,4, 7,2,2,0x0a,0,0,0,
    7,5,0x85,2,0,2,0,
    7,5,0x06,2,0,2,0,
};

static void descriptors(void)
{
    uint8_t d[sizeof(configuration)];
    memcpy(d, configuration, sizeof(d));
    d[2] = sizeof(d); d[3] = sizeof(d) >> 8;
    ecm_descriptor_t e;
    assert(ecm_parse_config(d, sizeof(d), &e));
    assert(e.control_interface == 3 && e.data_interface == 7 && e.data_alt == 2);
    assert(e.bulk_in == 0x85 && e.bulk_out == 6 && e.notification == 0x83);
    assert(e.in_mps == 512 && e.out_mps == 512 && e.mac_string == 4 && e.max_segment == 1514);
    for (size_t n = 0; n < sizeof(d); ++n) assert(!ecm_parse_config(d, n, &e));
    /* Missing/wrong CDC Union must never claim the adjacent unrelated interface. */
    d[36] = 4; assert(!ecm_parse_config(d, sizeof(d), &e)); d[36] = 7;
    d[24] = 0x0d; assert(!ecm_parse_config(d, sizeof(d), &e)); d[24] = 6; /* NCM */
    d[9] = 0; assert(!ecm_parse_config(d, sizeof(d), &e)); d[9] = 9;
    d[2] = 8; assert(!ecm_parse_config(d, sizeof(d), &e));
    assert(!ecm_parse_config(NULL, sizeof(d), &e));
}

static void mac_strings(void)
{
    uint8_t d[26] = {26,3};
    const char *text = "02aBcD001122";
    for (size_t i = 0; i < 12; ++i) d[2+i*2] = text[i];
    uint8_t mac[6];
    assert(ecm_parse_mac(d, sizeof(d), mac));
    const uint8_t expected[] = {2,0xab,0xcd,0,0x11,0x22};
    assert(!memcmp(mac, expected, 6));
    for (size_t n = 0; n < sizeof(d); ++n) assert(!ecm_parse_mac(d, n, mac));
    d[3] = 1; assert(!ecm_parse_mac(d, sizeof(d), mac)); d[3] = 0;
    d[2] = 'Z'; assert(!ecm_parse_mac(d, sizeof(d), mac)); d[2] = '0';
    d[4] = '3'; assert(!ecm_parse_mac(d, sizeof(d), mac));
    memset(d + 2, 0, 24);
    for (size_t i = 0; i < 12; ++i) d[2+i*2] = '0';
    assert(!ecm_parse_mac(d, sizeof(d), mac));
}

static void frame_bounds(void)
{
    uint8_t frame[2048] = {0};
    frame[6] = 2; frame[12] = 8;
    assert(bridge_frame_can_learn(frame, 14));
    assert(bridge_frame_valid(frame, 1514));
    assert(!bridge_frame_valid(frame, 1515));
    assert(!bridge_frame_valid(frame, 13));
    frame[12] = 0x88; frame[13] = 0xcc; /* LLDP from a switch cannot choose the client. */
    assert(!bridge_frame_can_learn(frame, 60));
    frame[12] = 0x81; frame[13] = 0;
    assert(!bridge_frame_valid(frame, 60));
    frame[6] = 0xff; assert(!bridge_frame_valid(frame, 60));
    assert(!bridge_frame_valid(NULL, 60));
}

static void random_inputs(void)
{
    uint8_t d[1024], mac[6];
    ecm_descriptor_t e;
    srand(1701);
    for (unsigned i = 0; i < 30000; ++i) {
        size_t n = rand() % sizeof(d);
        for (size_t j = 0; j < n; ++j) d[j] = rand();
        if (i % 2 && n >= 9) { d[0] = 9; d[1] = 2; d[2] = n; d[3] = n >> 8; }
        ecm_parse_config(d, n, &e);
        ecm_parse_mac(d, n, mac);
        bridge_frame_valid(d, n);
    }
}

int main(void)
{
    descriptors(); mac_strings(); frame_bounds(); random_inputs();
    puts("ECM descriptor, MAC, frame bounds and random-input checks passed");
}
