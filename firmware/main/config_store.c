#include "config_store.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "config";
static const char *NVS_NS = "bridge";
static const char *NVS_KEY_V1 = "cfg_v1";  /* legacy: Wi-Fi profiles only */
static const char *NVS_KEY_V2 = "cfg_v2";  /* current: + runtime modes */

static void modes_defaults(modes_config_t *m)
{
    memset(m, 0, sizeof(*m));
    /* CDC-ACM management console on by default (seq 1 = oldest). */
    m->usb_enabled[USB_FUNC_ACM] = 1;
    m->usb_seq[USB_FUNC_ACM] = 1;
    m->seq_next = 2;
}

void config_defaults(bridge_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->active = CFG_ACTIVE_NONE;
    modes_defaults(&cfg->modes);

#ifdef CONFIG_BRIDGE_WIFI_SSID
    if (CONFIG_BRIDGE_WIFI_SSID[0] != '\0') {
        strncpy(cfg->profiles[0].ssid, CONFIG_BRIDGE_WIFI_SSID, CFG_SSID_MAX - 1);
        strncpy(cfg->profiles[0].password, CONFIG_BRIDGE_WIFI_PASSWORD, CFG_PASS_MAX - 1);
        cfg->profile_count = 1;
        cfg->active = 0;
    }
#endif
}

static void validate_loaded(bridge_config_t *cfg)
{
    if (cfg->profile_count > CFG_PROFILE_MAX) {
        ESP_LOGW(TAG, "Profile count corrupt; resetting");
        config_defaults(cfg);
        return;
    }
    if (cfg->active != CFG_ACTIVE_NONE && cfg->active >= cfg->profile_count) {
        cfg->active = cfg->profile_count ? 0 : CFG_ACTIVE_NONE;
    }
    if (cfg->modes.seq_next == 0) {
        cfg->modes.seq_next = 1;
    }
}

void config_load(bridge_config_t *cfg)
{
    config_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved config; using defaults");
        return;
    }

    /* Preferred: full v2 blob (Wi-Fi + modes). */
    size_t len = sizeof(*cfg);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_V2, cfg, &len);
    if (err == ESP_OK && len == sizeof(*cfg)) {
        nvs_close(h);
        validate_loaded(cfg);
        ESP_LOGI(TAG, "Loaded v2 config: %u profile(s), active=%u",
                 cfg->profile_count, cfg->active == CFG_ACTIVE_NONE ? 255 : cfg->active);
        return;
    }

    /* Migrate: legacy v1 blob = Wi-Fi prefix only; keep default modes. */
    const size_t v1_len = offsetof(bridge_config_t, modes);
    len = sizeof(*cfg);
    err = nvs_get_blob(h, NVS_KEY_V1, cfg, &len);
    nvs_close(h);

    if (err == ESP_OK && len == v1_len) {
        modes_defaults(&cfg->modes);  /* blob had no modes region */
        validate_loaded(cfg);
        ESP_LOGW(TAG, "Migrated legacy v1 config to v2 defaults");
        return;
    }

    ESP_LOGW(TAG, "Saved config invalid (%s); using defaults", esp_err_to_name(err));
    config_defaults(cfg);
}

bool config_save(const bridge_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, NVS_KEY_V2, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Config saved");
    return true;
}

bool config_usb_func_enabled(const bridge_config_t *cfg, usb_func_t func)
{
    if (func >= USB_FUNC_COUNT) {
        return false;
    }
    return cfg->modes.usb_enabled[func] != 0;
}

void config_usb_func_set(bridge_config_t *cfg, usb_func_t func, bool enable)
{
    if (func >= USB_FUNC_COUNT) {
        return;
    }
    if (enable) {
        cfg->modes.usb_enabled[func] = 1;
        cfg->modes.usb_seq[func] = cfg->modes.seq_next++;
        if (cfg->modes.seq_next == 0) {
            cfg->modes.seq_next = 1;  /* wrap guard */
        }
    } else {
        cfg->modes.usb_enabled[func] = 0;
        cfg->modes.usb_seq[func] = 0;
    }
}

bool config_share_enabled(const bridge_config_t *cfg)
{
    return cfg->modes.share_enabled != 0;
}

void config_share_set(bridge_config_t *cfg, bool enable)
{
    cfg->modes.share_enabled = enable ? 1 : 0;
}

bool config_nat_wanted(const bridge_config_t *cfg)
{
    return config_share_enabled(cfg) || config_usb_func_enabled(cfg, USB_FUNC_HID);
}

void config_clear_modes(bridge_config_t *cfg)
{
    modes_defaults(&cfg->modes);
    ESP_LOGI(TAG, "Runtime modes cleared to defaults");
}

const char *config_active_ssid(const bridge_config_t *cfg)
{
    if (cfg->active == CFG_ACTIVE_NONE || cfg->active >= cfg->profile_count) {
        return "";
    }
    return cfg->profiles[cfg->active].ssid;
}

const char *config_active_pass(const bridge_config_t *cfg)
{
    if (cfg->active == CFG_ACTIVE_NONE || cfg->active >= cfg->profile_count) {
        return "";
    }
    return cfg->profiles[cfg->active].password;
}

int config_find_profile(const bridge_config_t *cfg, const char *ssid)
{
    for (int i = 0; i < cfg->profile_count; i++) {
        if (strcmp(cfg->profiles[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

int config_add_profile(bridge_config_t *cfg, const char *ssid)
{
    int existing = config_find_profile(cfg, ssid);
    if (existing >= 0) {
        return existing;
    }
    if (cfg->profile_count >= CFG_PROFILE_MAX) {
        return -1;
    }
    int i = cfg->profile_count++;
    memset(&cfg->profiles[i], 0, sizeof(cfg->profiles[i]));
    strncpy(cfg->profiles[i].ssid, ssid, CFG_SSID_MAX - 1);
    if (cfg->active == CFG_ACTIVE_NONE) {
        cfg->active = (uint8_t)i;
    }
    return i;
}

void config_del_profile(bridge_config_t *cfg, int index)
{
    if (index < 0 || index >= cfg->profile_count) {
        return;
    }
    for (int i = index; i < cfg->profile_count - 1; i++) {
        cfg->profiles[i] = cfg->profiles[i + 1];
    }
    cfg->profile_count--;
    memset(&cfg->profiles[cfg->profile_count], 0, sizeof(wifi_profile_t));

    if (cfg->profile_count == 0) {
        cfg->active = CFG_ACTIVE_NONE;
    } else if (cfg->active == index) {
        cfg->active = 0;
    } else if (cfg->active != CFG_ACTIVE_NONE && cfg->active > index) {
        cfg->active--;
    }
}

void config_clear_wifi(bridge_config_t *cfg)
{
    /* Explicit user reset: do not re-seed compile-time menuconfig SSID.
     * A factory reset also returns all runtime modes to defaults. */
    memset(cfg, 0, sizeof(*cfg));
    cfg->active = CFG_ACTIVE_NONE;
    modes_defaults(&cfg->modes);
    ESP_LOGI(TAG, "Wi-Fi profiles and modes cleared");
}
