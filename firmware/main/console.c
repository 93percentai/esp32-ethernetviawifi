#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "provisioning.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "wifi_mgr.h"

static const char *TAG = "console";
static bridge_config_t *s_cfg;
static char s_line[160];
static size_t s_line_len;
static bool s_greeted;

static void cdc_write(const char *s)
{
    if (!tud_cdc_n_connected(TINYUSB_CDC_ACM_0)) {
        return;
    }
    size_t len = strlen(s);
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)s, len);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

void console_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cdc_write(buf);
}

static const char *state_str(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_IDLE: return "idle (no credentials)";
    case WIFI_MGR_CONNECTING: return "associating";
    case WIFI_MGR_CONNECTED: return "connected";
    case WIFI_MGR_DISCONNECTED: return "disconnected";
    case WIFI_MGR_NO_AP: return "no AP found";
    case WIFI_MGR_BAD_AUTH: return "bad auth";
    case WIFI_MGR_SCANNING: return "scanning";
    case WIFI_MGR_PROVISIONING: return "softap portal";
    default: return "?";
    }
}

static void print_status(void)
{
    wifi_mgr_status_t st;
    bridge_stats_t stats;
    wifi_mgr_get_status(&st);
    bridge_get_stats(&stats);

    char down[24], up[24], rdown[24], rup[24];
    bridge_format_bytes(stats.bytes_to_host, down, sizeof(down));
    bridge_format_bytes(stats.bytes_to_wifi, up, sizeof(up));
    bridge_format_rate(stats.rate_to_host_bps, rdown, sizeof(rdown));
    bridge_format_rate(stats.rate_to_wifi_bps, rup, sizeof(rup));

    console_printf("\r\n-- esp32-ethernetviawifi --\r\n");
    console_printf("  profiles:   %u saved (active: %s)\r\n",
                   s_cfg->profile_count,
                   s_cfg->active == CFG_ACTIVE_NONE ? "none" : "yes");
    console_printf("  ssid:       %s\r\n",
                   config_active_ssid(s_cfg)[0] ? config_active_ssid(s_cfg) : "(unset)");
    console_printf("  pass:       %s\r\n",
                   config_active_pass(s_cfg)[0] ? "set" : "unset");
    console_printf("  status:     %s\r\n", state_str(st.state));
    if (st.state == WIFI_MGR_CONNECTED) {
        console_printf("  rssi:       %d dBm  ch=%u\r\n", st.rssi, st.channel);
    }
    console_printf("  mac:        %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                   st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
    console_printf("  download:   %s  (%s)\r\n", down, rdown);
    console_printf("  upload:     %s  (%s)\r\n", up, rup);
    console_printf("  frames:     ->host=%lu ->wifi=%lu drop_tx=%lu drop_rx=%lu refl=%lu\r\n",
                   (unsigned long)stats.frames_to_host,
                   (unsigned long)stats.frames_to_wifi,
                   (unsigned long)stats.drop_tx,
                   (unsigned long)stats.drop_rx,
                   (unsigned long)stats.drop_refl);
    console_printf("(set|scan|list|use|del|save|status|help) # ");
}

static void cmd_help(void)
{
    console_printf(
        "Commands:\r\n"
        "  set ssid <name>     set active profile SSID and re-associate\r\n"
        "  set pass <pass>     set WPA passphrase (blank = open)\r\n"
        "  list                list saved profiles\r\n"
        "  use <n>             activate profile n (1-based)\r\n"
        "  del <n>             delete profile n\r\n"
        "  scan                scan nearby APs\r\n"
        "  join <n>            stage scanned AP #n as active SSID\r\n"
        "  save                persist profiles to NVS\r\n"
        "  status              show link + traffic stats\r\n"
        "  resetstats          clear byte/frame counters\r\n"
        "  help                this text\r\n"
        "\r\n"
        "With no SSID, SoftAP '%s' + http://192.168.1.1 is also available.\r\n",
        PROV_SOFTAP_SSID);
}

static wifi_scan_result_t s_scan[24];
static int s_scan_count;

static void cmd_scan(void)
{
    console_printf("[*] scanning...\r\n");
    s_scan_count = wifi_mgr_scan(s_scan, 24, !provisioning_is_active());
    console_printf("  networks (%d):\r\n", s_scan_count);
    for (int i = 0; i < s_scan_count; i++) {
        console_printf("   %2d  %-24s  %4d dBm  %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                       i + 1, s_scan[i].ssid, s_scan[i].rssi,
                       s_scan[i].bssid[0], s_scan[i].bssid[1], s_scan[i].bssid[2],
                       s_scan[i].bssid[3], s_scan[i].bssid[4], s_scan[i].bssid[5]);
    }
}

static void ensure_active_profile(void)
{
    if (s_cfg->active == CFG_ACTIVE_NONE) {
        if (s_cfg->profile_count == 0) {
            config_add_profile(s_cfg, "");
        }
        s_cfg->active = 0;
    }
}

static void handle_line(char *line)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (!*line) {
        print_status();
        return;
    }

    /* lowercase first token for case-insensitive commands */
    char *cmd = line;
    char *rest = strchr(line, ' ');
    if (rest) {
        *rest++ = '\0';
        while (*rest == ' ') {
            rest++;
        }
    } else {
        rest = "";
    }
    for (char *p = cmd; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (char)(*p - 'A' + 'a');
        }
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
        return;
    } else if (strcmp(cmd, "scan") == 0) {
        cmd_scan();
    } else if (strcmp(cmd, "join") == 0) {
        int n = atoi(rest);
        if (n < 1 || n > s_scan_count) {
            console_printf("[!] join <1..%d>\r\n", s_scan_count > 0 ? s_scan_count : 0);
        } else {
            int idx = config_add_profile(s_cfg, s_scan[n - 1].ssid);
            if (idx < 0) {
                console_printf("[!] profile list full\r\n");
            } else {
                s_cfg->active = (uint8_t)idx;
                s_cfg->profiles[idx].password[0] = '\0';
                console_printf("[*] staged '%s' — set pass <password> then save\r\n",
                               s_scan[n - 1].ssid);
                provisioning_apply_or_start(s_cfg);
            }
        }
    } else if (strcmp(cmd, "list") == 0) {
        console_printf("  profiles (%u/%u):\r\n", s_cfg->profile_count, CFG_PROFILE_MAX);
        for (int i = 0; i < s_cfg->profile_count; i++) {
            console_printf("   %d%c %s%s\r\n", i + 1,
                           s_cfg->active == i ? '*' : ' ',
                           s_cfg->profiles[i].ssid,
                           s_cfg->profiles[i].password[0] ? "" : "  (open)");
        }
        console_printf("  (* = active)\r\n");
    } else if (strcmp(cmd, "use") == 0) {
        int n = atoi(rest);
        if (n < 1 || n > s_cfg->profile_count) {
            console_printf("[!] use <1..%u>\r\n", s_cfg->profile_count);
        } else {
            s_cfg->active = (uint8_t)(n - 1);
            console_printf("[*] applying — re-associating\r\n");
            provisioning_apply_or_start(s_cfg);
        }
    } else if (strcmp(cmd, "del") == 0) {
        int n = atoi(rest);
        if (n < 1 || n > s_cfg->profile_count) {
            console_printf("[!] del <1..%u>\r\n", s_cfg->profile_count);
        } else {
            config_del_profile(s_cfg, n - 1);
            provisioning_apply_or_start(s_cfg);
            console_printf("[*] deleted\r\n");
        }
    } else if (strcmp(cmd, "save") == 0) {
        console_printf(config_save(s_cfg) ? "[*] saved to NVS\r\n" : "[!] save failed\r\n");
    } else if (strcmp(cmd, "resetstats") == 0) {
        bridge_reset_stats();
        console_printf("[*] stats cleared\r\n");
    } else if (strcmp(cmd, "set") == 0) {
        char *key = rest;
        char *val = strchr(rest, ' ');
        if (val) {
            *val++ = '\0';
            while (*val == ' ') {
                val++;
            }
        } else {
            val = "";
        }
        for (char *p = key; *p; p++) {
            if (*p >= 'A' && *p <= 'Z') {
                *p = (char)(*p - 'A' + 'a');
            }
        }
        if (strcmp(key, "ssid") == 0) {
            ensure_active_profile();
            strncpy(s_cfg->profiles[s_cfg->active].ssid, val, CFG_SSID_MAX - 1);
            console_printf("[*] applying — re-associating\r\n");
            provisioning_apply_or_start(s_cfg);
        } else if (strcmp(key, "pass") == 0) {
            ensure_active_profile();
            strncpy(s_cfg->profiles[s_cfg->active].password, val, CFG_PASS_MAX - 1);
            console_printf("[*] applying — re-associating\r\n");
            provisioning_apply_or_start(s_cfg);
        } else {
            console_printf("[!] set ssid|pass <value>\r\n");
        }
    } else {
        console_printf("[!] unknown command (try help)\r\n");
    }

    print_status();
}

static void on_cdc_rx(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buf[64];
    size_t rx;
    if (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &rx) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < rx; i++) {
        char c = (char)buf[i];
        if (c == '\r' || c == '\n') {
            if (s_line_len > 0) {
                s_line[s_line_len] = '\0';
                cdc_write("\r\n");
                handle_line(s_line);
                s_line_len = 0;
            }
            continue;
        }
        if (c == 0x7f || c == '\b') {
            if (s_line_len > 0) {
                s_line_len--;
                cdc_write("\b \b");
            }
            continue;
        }
        if (c >= 32 && c < 127 && s_line_len + 1 < sizeof(s_line)) {
            s_line[s_line_len++] = c;
            char echo[2] = {c, 0};
            cdc_write(echo);
        }
    }
}

static void console_task(void *arg)
{
    (void)arg;
    while (true) {
        if (tud_cdc_n_connected(TINYUSB_CDC_ACM_0)) {
            if (!s_greeted) {
                s_greeted = true;
                print_status();
            }
        } else {
            s_greeted = false;
            s_line_len = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t console_init(bridge_config_t *cfg)
{
    s_cfg = cfg;

    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 256,
        .callback_rx = on_cdc_rx,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));
    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "CDC-ACM management console ready");
    return ESP_OK;
}
