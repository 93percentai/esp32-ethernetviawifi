#include "provisioning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "dns_server.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "wifi_mgr.h"
#include "form_codec.h"

static const char *TAG = "provisioning";

#define PROV_SCAN_MAX       24
#define PROV_TEST_TIMEOUT_MS 20000
#define PROV_HTML_MAX       16384

static bridge_config_t *s_cfg;
static atomic_bool s_active;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_httpd;
static dns_server_handle_t s_dns;
static wifi_scan_result_t s_scan[PROV_SCAN_MAX];
static atomic_int s_scan_count;
static char s_status_msg[96];
static SemaphoreHandle_t s_lock;
static atomic_bool s_testing;
static bool s_netif_ready;
static atomic_int s_phase;
static atomic_int s_clients;
static bool s_ap_events_registered;

static void set_status_msg(const char *msg)
{
    strncpy(s_status_msg, msg ? msg : "", sizeof(s_status_msg) - 1);
    s_status_msg[sizeof(s_status_msg) - 1] = '\0';
}

static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_clients++;
        ESP_LOGI(TAG, "Portal client join (clients=%d)", s_clients);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_clients > 0) {
            s_clients--;
        }
        ESP_LOGI(TAG, "Portal client leave (clients=%d)", s_clients);
    }
    (void)data;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char *page = malloc(PROV_HTML_MAX);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t used = 0;
    used += snprintf(page + used, PROV_HTML_MAX - used,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>WiFi Setup</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;margin:1.25rem;background:#0f1419;color:#e7ecf1}"
        "h1{font-size:1.25rem;margin:0 0 .5rem}"
        "p{color:#9aa7b5;font-size:.9rem}"
        "label{display:block;margin:.75rem 0 .25rem;font-size:.85rem;color:#c5d0da}"
        "select,input{width:100%%;max-width:28rem;box-sizing:border-box;padding:.55rem .65rem;"
        "border:1px solid #2a3540;border-radius:6px;background:#1a222b;color:#e7ecf1}"
        "button{margin-top:1rem;padding:.65rem 1.1rem;border:0;border-radius:6px;"
        "background:#1f8a70;color:#fff;font-weight:600;cursor:pointer}"
        "button:disabled{opacity:.5}"
        ".msg{margin:.75rem 0;padding:.6rem .75rem;border-radius:6px;background:#1a222b;"
        "border-left:3px solid #3d9cf0;color:#d7e6f5;max-width:28rem}"
        ".err{border-left-color:#e35d6a}"
        ".ok{border-left-color:#1f8a70}"
        "</style></head><body>"
        "<h1>Configure Wi‑Fi</h1>"
        "<p>Connect this device to your network. SoftAP <b>%s</b> stops after a successful test.</p>",
        PROV_SOFTAP_SSID);

    if (s_status_msg[0]) {
        const char *cls = (strstr(s_status_msg, "Connected") || strstr(s_status_msg, "Success"))
                              ? "msg ok"
                              : (strstr(s_status_msg, "Failed") || strstr(s_status_msg, "failed") ||
                                 strstr(s_status_msg, "timeout") || strstr(s_status_msg, "Timeout"))
                                    ? "msg err"
                                    : "msg";
        char esc[sizeof(s_status_msg) * 6];
        html_escape(s_status_msg, esc, sizeof(esc));
        used += snprintf(page + used, PROV_HTML_MAX - used, "<div class=\"%s\">%s</div>", cls, esc);
    }

    used += snprintf(page + used, PROV_HTML_MAX - used,
        "<form method=\"POST\" action=\"/connect\" id=\"f\">"
        "<label for=\"ssid\">Network (SSID)</label>"
        "<select name=\"ssid\" id=\"ssid\">");

    if (s_scan_count == 0) {
        used += snprintf(page + used, PROV_HTML_MAX - used,
                         "<option value=\"\">(no networks found — use custom)</option>");
    } else {
        for (int i = 0; i < s_scan_count && used + 2 * CFG_SSID_MAX * 6 + 96 < PROV_HTML_MAX; i++) {
            char esc[CFG_SSID_MAX * 6];
            html_escape(s_scan[i].ssid, esc, sizeof(esc));
            const char *lock = (s_scan[i].authmode == WIFI_AUTH_OPEN) ? "" : " *";
            used += snprintf(page + used, PROV_HTML_MAX - used,
                             "<option value=\"%s\">%s (%d dBm)%s</option>",
                             esc, esc, (int)s_scan[i].rssi, lock);
        }
    }

    used += snprintf(page + used, PROV_HTML_MAX - used,
        "</select>"
        "<label for=\"custom\">Or type SSID</label>"
        "<input name=\"custom\" id=\"custom\" maxlength=\"32\" placeholder=\"optional override\">"
        "<label for=\"pass\">Password</label>"
        "<input name=\"pass\" id=\"pass\" type=\"password\" maxlength=\"63\" "
        "placeholder=\"leave blank for open networks\">"
        "<button type=\"submit\" id=\"go\"%s>Test &amp; Save</button>"
        "</form>"
        "<p style=\"margin-top:1.25rem\">Portal: http://%d.%d.%d.%d</p>"
        "<script>document.getElementById('f').onsubmit=function(){"
        "document.getElementById('go').disabled=true;document.getElementById('go').textContent='Testing…';};"
        "</script></body></html>",
        s_testing ? " disabled" : "",
        PROV_SOFTAP_IP_A, PROV_SOFTAP_IP_B, PROV_SOFTAP_IP_C, PROV_SOFTAP_IP_D);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, page, used);
    free(page);
    return err;
}

static void finish_success_task(void *arg)
{
    (void)arg;
    /* Let the HTTP response flush before tearing the SoftAP down. */
    vTaskDelay(pdMS_TO_TICKS(1200));

    char ssid[CFG_SSID_MAX];
    char pass[CFG_PASS_MAX];
    strncpy(ssid, s_cfg->profiles[s_cfg->active].ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    strncpy(pass, s_cfg->profiles[s_cfg->active].password, sizeof(pass) - 1);
    pass[sizeof(pass) - 1] = '\0';

    ESP_LOGI(TAG, "Provisioning succeeded for '%s' — stopping SoftAP", ssid);
    provisioning_stop();
    wifi_mgr_apply(s_cfg);
    vTaskDelete(NULL);
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    if (s_testing) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "Test already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad form");
        return ESP_FAIL;
    }

    char body[513];
    size_t received = 0;
    while (received < req->content_len) {
        int chunk = httpd_req_recv(req, body + received, req->content_len - received);
        if (chunk <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete form");
            return ESP_FAIL;
        }
        received += chunk;
    }
    body[received] = '\0';
    if (memchr(body, 0, received)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
        return ESP_FAIL;
    }
    char ssid[CFG_SSID_MAX] = {0};
    char custom[CFG_SSID_MAX] = {0};
    char pass[CFG_PASS_MAX] = {0};
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) ||
        !form_get(body, "custom", custom, sizeof(custom)) ||
        !form_get(body, "pass", pass, sizeof(pass))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or overlong Wi-Fi settings");
        return ESP_FAIL;
    }
    if (custom[0]) {
        memcpy(ssid, custom, sizeof(ssid) - 1);
        ssid[sizeof(ssid) - 1] = '\0';
    }
    if (!ssid[0]) {
        set_status_msg("Please select or enter an SSID.");
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, "Redirect", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
        return ESP_FAIL;
    }
    s_testing = true;
    s_phase = PROV_PHASE_TESTING;
    set_status_msg("Testing connection…");
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Portal submitted SSID '%s' — testing", ssid);
    esp_err_t terr = wifi_mgr_test_connect(ssid, pass, PROV_TEST_TIMEOUT_MS);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_testing = false;
    if (terr == ESP_OK) {
        int idx = config_add_profile(s_cfg, ssid);
        if (idx < 0) {
            s_phase = PROV_PHASE_PORTAL;
            set_status_msg("Failed: profile list full.");
            xSemaphoreGive(s_lock);
            httpd_resp_set_status(req, "303 See Other");
            httpd_resp_set_hdr(req, "Location", "/");
            httpd_resp_send(req, "Redirect", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        s_cfg->active = (uint8_t)idx;
        memcpy(s_cfg->profiles[idx].password, pass, CFG_PASS_MAX - 1);
        s_cfg->profiles[idx].password[CFG_PASS_MAX - 1] = '\0';
        if (!config_save(s_cfg)) {
            s_phase = PROV_PHASE_PORTAL;
            set_status_msg("Failed: settings could not be saved. Please retry.");
            wifi_mgr_set_state(WIFI_MGR_PROVISIONING);
            xSemaphoreGive(s_lock);
            return root_get_handler(req);
        }
        s_phase = PROV_PHASE_SUCCESS;
        set_status_msg("Connected — saving and closing setup network…");
        xSemaphoreGive(s_lock);

        const char *ok_page =
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<meta http-equiv=\"refresh\" content=\"2;url=/\">"
            "<title>Connected</title></head><body style=\"font-family:system-ui;padding:1.5rem\">"
            "<h1>Connected</h1><p>Credentials saved. Setup network is shutting down…</p>"
            "</body></html>";
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, ok_page, HTTPD_RESP_USE_STRLEN);
        xTaskCreate(finish_success_task, "prov_done", 4096, NULL, 5, NULL);
        return ESP_OK;
    }

    if (terr == ESP_ERR_TIMEOUT) {
        set_status_msg("Failed: connection timed out. Check SSID/password and try again.");
    } else {
        set_status_msg("Failed: could not associate (wrong password or AP unreachable).");
    }
    s_phase = PROV_PHASE_PORTAL;
    wifi_mgr_set_state(WIFI_MGR_PROVISIONING);
    xSemaphoreGive(s_lock);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirect", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t start_httpd(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_httpd = NULL;
        return ESP_FAIL;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t connect = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = connect_post_handler,
    };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &connect);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_handler);
    return ESP_OK;
}

static void configure_softap_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, PROV_SOFTAP_IP_A, PROV_SOFTAP_IP_B, PROV_SOFTAP_IP_C, PROV_SOFTAP_IP_D);
    IP4_ADDR(&ip_info.gw, PROV_SOFTAP_IP_A, PROV_SOFTAP_IP_B, PROV_SOFTAP_IP_C, PROV_SOFTAP_IP_D);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));

    char uri[] = "http://192.168.1.1";
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                               uri, strlen(uri)));

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));
}

static esp_err_t start_softap(void)
{
    if (!s_netif_ready) {
        ESP_ERROR_CHECK(esp_netif_init());
        s_netif_ready = true;
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            return ESP_FAIL;
        }
    }

    configure_softap_ip(s_ap_netif);

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, PROV_SOFTAP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(PROV_SOFTAP_SSID);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.ssid_hidden = 0;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    if (!s_ap_events_registered) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                                   on_ap_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                                   on_ap_event, NULL));
        s_ap_events_registered = true;
    }

    ESP_LOGI(TAG, "SoftAP '%s' at %d.%d.%d.%d",
             PROV_SOFTAP_SSID,
             PROV_SOFTAP_IP_A, PROV_SOFTAP_IP_B, PROV_SOFTAP_IP_C, PROV_SOFTAP_IP_D);
    return ESP_OK;
}

esp_err_t provisioning_start(bridge_config_t *cfg)
{
    if (s_active || s_phase != PROV_PHASE_IDLE) {
        return ESP_OK;
    }
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = cfg;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_testing = false;
    s_clients = 0;
    s_phase = PROV_PHASE_SCANNING;
    set_status_msg("Scanning nearby networks…");

    wifi_mgr_set_suppress_bridge(true);
    wifi_mgr_set_state(WIFI_MGR_PROVISIONING);

    ESP_LOGI(TAG, "No SSID configured — scanning before SoftAP portal");
    s_scan_count = wifi_mgr_scan(s_scan, PROV_SCAN_MAX, false);
    ESP_LOGI(TAG, "Captured %d SSID(s)", s_scan_count);

    wifi_mgr_set_state(WIFI_MGR_PROVISIONING);
    set_status_msg("Pick a network, enter the password, then Test & Save.");

    esp_err_t err = start_softap();
    if (err != ESP_OK) {
        s_phase = PROV_PHASE_IDLE;
        wifi_mgr_set_suppress_bridge(false);
        wifi_mgr_set_state(WIFI_MGR_IDLE);
        return err;
    }

    err = start_httpd();
    if (err != ESP_OK) {
        s_phase = PROV_PHASE_IDLE;
        esp_wifi_set_mode(WIFI_MODE_STA);
        wifi_mgr_set_suppress_bridge(false);
        wifi_mgr_set_state(WIFI_MGR_IDLE);
        return err;
    }

    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns_cfg);

    s_phase = PROV_PHASE_PORTAL;
    s_active = true;
    wifi_mgr_set_state(WIFI_MGR_PROVISIONING);
    ESP_LOGI(TAG, "Captive portal ready — join '%s' and open http://192.168.1.1",
             PROV_SOFTAP_SSID);
    return ESP_OK;
}

void provisioning_stop(void)
{
    if (!s_active && s_phase == PROV_PHASE_IDLE) {
        return;
    }

    ESP_LOGI(TAG, "Stopping SoftAP captive portal");
    s_testing = false;
    s_clients = 0;

    if (s_dns) {
        stop_dns_server(s_dns);
        s_dns = NULL;
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_mgr_set_suppress_bridge(false);
    /* Remain active until HTTP/DNS/AP teardown finishes, so touch cannot
     * start a second portal during the success worker's asynchronous stop. */
    s_phase = PROV_PHASE_IDLE;
    s_active = false;
}

bool provisioning_is_active(void)
{
    return s_active;
}

void provisioning_get_lcd_status(provisioning_lcd_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->phase = s_phase;
    out->active = (s_phase != PROV_PHASE_IDLE);
    out->scan_count = s_scan_count;
    out->clients = s_clients;

    switch (s_phase) {
    case PROV_PHASE_SCANNING:
        snprintf(out->detail, sizeof(out->detail), "Scanning...");
        break;
    case PROV_PHASE_TESTING:
        snprintf(out->detail, sizeof(out->detail), "Testing WiFi");
        break;
    case PROV_PHASE_SUCCESS:
        snprintf(out->detail, sizeof(out->detail), "Saved - closing");
        break;
    case PROV_PHASE_PORTAL:
        if (s_clients > 0) {
            snprintf(out->detail, sizeof(out->detail), "%d client(s)", s_clients);
        } else {
            snprintf(out->detail, sizeof(out->detail), "Join SoftAP");
        }
        break;
    default:
        out->detail[0] = '\0';
        break;
    }
}

esp_err_t provisioning_apply_or_start(bridge_config_t *cfg)
{
    if (config_active_ssid(cfg)[0]) {
        if (s_active) {
            provisioning_stop();
        }
        return wifi_mgr_apply(cfg);
    }

    /* Clear any prior STA attempt, then open the portal. */
    wifi_mgr_apply(cfg);
    return provisioning_start(cfg);
}
