#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CFG_SSID_MAX     33
#define CFG_PASS_MAX     64
#define CFG_PROFILE_MAX  8
#define CFG_ACTIVE_NONE  0xFF

typedef struct {
    char ssid[CFG_SSID_MAX];
    char password[CFG_PASS_MAX];
} wifi_profile_t;

typedef struct {
    uint8_t profile_count;
    uint8_t active;          /* index or CFG_ACTIVE_NONE */
    uint8_t debug_enabled;
    wifi_profile_t profiles[CFG_PROFILE_MAX];
} bridge_config_t;

void config_defaults(bridge_config_t *cfg);
void config_load(bridge_config_t *cfg);
bool config_save(const bridge_config_t *cfg);

const char *config_active_ssid(const bridge_config_t *cfg);
const char *config_active_pass(const bridge_config_t *cfg);

int config_find_profile(const bridge_config_t *cfg, const char *ssid);
int config_add_profile(bridge_config_t *cfg, const char *ssid);
void config_del_profile(bridge_config_t *cfg, int index);

/* Wipe all Wi-Fi profiles in RAM (does not touch NVS until config_save). */
void config_clear_wifi(bridge_config_t *cfg);
