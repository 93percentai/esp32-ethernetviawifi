#include "httpd_share.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bridge.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hid.h"
#include "net_tether.h"
#include "sdcard.h"
#include "usb_gadget.h"
#include "wifi_mgr.h"

static const char *TAG = "httpd_share";
static httpd_handle_t s_server;
static bool s_storage_share_enabled;
static volatile uint32_t s_web_ops;
static volatile uint64_t s_web_read_bytes;
static volatile uint64_t s_web_write_bytes;
static volatile int64_t s_web_last_io_us = -1;

#define XFER_BUF_SZ 2048
#define MAX_PATH_SZ 320
#define WEB_ACTIVE_WINDOW_MS 1500

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < out_sz; i++) {
        if (in[i] == '%' && hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0) {
            out[o++] = (char)((hexval(in[i + 1]) << 4) | hexval(in[i + 2]));
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

/* Build a filesystem path under the SD base for a decoded relative URL path.
 * Rejects parent traversal. rel may be "" or "/" for the root. */
static bool build_fs_path(char *out, size_t out_sz, const char *rel)
{
    if (strstr(rel, "..")) {
        return false;
    }
    const char *base = sdcard_base_path();
    if (!rel || rel[0] == '\0') {
        snprintf(out, out_sz, "%s", base);
        return true;
    }
    if (rel[0] != '/') {
        snprintf(out, out_sz, "%s/%s", base, rel);
    } else {
        snprintf(out, out_sz, "%s%s", base, rel);
    }
    /* Strip trailing slash (except bare base). */
    size_t l = strlen(out);
    while (l > strlen(base) + 0 && out[l - 1] == '/') {
        out[--l] = '\0';
    }
    return true;
}

static bool sd_ready(void)
{
    return s_storage_share_enabled && sdcard_present() && sdcard_owner() == SD_OWNER_ESP;
}

static void web_op_begin(void)
{
    s_web_ops++;
}

static void web_op_end(void)
{
    if (s_web_ops > 0) {
        s_web_ops--;
    }
}

static void web_io_add(bool write, size_t bytes)
{
    if (write) {
        s_web_write_bytes += bytes;
    } else {
        s_web_read_bytes += bytes;
    }
    s_web_last_io_us = esp_timer_get_time();
}

static uint32_t web_ms_since_io(void)
{
    if (s_web_last_io_us < 0) {
        return UINT32_MAX;
    }
    int64_t ms = (esp_timer_get_time() - s_web_last_io_us) / 1000;
    return ms < 0 ? 0 : (ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_sd_busy(httpd_req_t *req)
{
    return send_json(req, "503 Service Unavailable",
                     "{\"error\":\"sd_busy\",\"msg\":\"SD card is in use by USB storage\"}");
}

/* Read a query key, url-decode into out. Returns false if missing. */
static bool get_query(httpd_req_t *req, const char *key, char *out, size_t out_sz)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) {
        return false;
    }
    char *q = malloc(qlen);
    if (!q) {
        return false;
    }
    bool ok = false;
    if (httpd_req_get_url_query_str(req, q, qlen) == ESP_OK) {
        char raw[MAX_PATH_SZ];
        if (httpd_query_key_value(q, key, raw, sizeof(raw)) == ESP_OK) {
            url_decode(raw, out, out_sz);
            ok = true;
        }
    }
    free(q);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Device status + SD ownership control                               */
/* ------------------------------------------------------------------ */

static const char *wifi_state_name(wifi_mgr_state_t state)
{
    switch (state) {
    case WIFI_MGR_IDLE: return "idle";
    case WIFI_MGR_CONNECTING: return "connecting";
    case WIFI_MGR_CONNECTED: return "connected";
    case WIFI_MGR_DISCONNECTED: return "disconnected";
    case WIFI_MGR_NO_AP: return "no_ap";
    case WIFI_MGR_BAD_AUTH: return "bad_auth";
    case WIFI_MGR_SCANNING: return "scanning";
    case WIFI_MGR_PROVISIONING: return "provisioning";
    default: return "unknown";
    }
}

static void format_ip(uint32_t addr, char *out, size_t out_len)
{
    snprintf(out, out_len, "%u.%u.%u.%u",
             (unsigned)(addr & 0xff), (unsigned)((addr >> 8) & 0xff),
             (unsigned)((addr >> 16) & 0xff), (unsigned)((addr >> 24) & 0xff));
}

static esp_err_t api_status_get(httpd_req_t *req)
{
    sd_io_stats_t io;
    sdcard_get_io_stats(&io);
    hid_stats_t hid_stats;
    hid_get_stats(&hid_stats);
    bridge_stats_t bridge;
    bridge_get_stats(&bridge);
    wifi_mgr_status_t wifi;
    wifi_mgr_get_status(&wifi);

    const char *owner = "none";
    switch (sdcard_owner()) {
    case SD_OWNER_HOST: owner = "host"; break;
    case SD_OWNER_ESP:  owner = "esp";  break;
    default: owner = "none"; break;
    }

    char sta_ip[20] = "";
    char usb_ip[20] = "";
    if (wifi.has_ip) {
        format_ip(wifi.sta_ip, sta_ip, sizeof(sta_ip));
    }
    esp_netif_ip_info_t usb;
    net_tether_get_usb_ip(&usb);
    if (usb.ip.addr) {
        format_ip(usb.ip.addr, usb_ip, sizeof(usb_ip));
    }

    uint32_t web_ms = web_ms_since_io();
    bool web_active = s_web_ops > 0 || web_ms < WEB_ACTIVE_WINDOW_MS;
    char buf[1800];
    snprintf(buf, sizeof(buf),
             "{"
             "\"modes\":{\"ncm\":true,\"acm\":%s,\"msc\":%s,\"hid\":%s,"
             "\"share\":%s,\"http_server\":%s,\"network\":\"%s\"},"
             "\"wifi\":{\"state\":\"%s\",\"ssid\":\"%.32s\",\"rssi\":%d,"
             "\"sta_ip\":\"%s\",\"usb_ip\":\"%s\"},"
             "\"bridge\":{\"down_bytes\":%llu,\"up_bytes\":%llu,"
             "\"down_bps\":%.0f,\"up_bps\":%.0f,"
             "\"frames_to_host\":%lu,\"frames_to_wifi\":%lu,"
             "\"drop_tx\":%lu,\"drop_rx\":%lu,\"drop_reflected\":%lu},"
             "\"sd\":{\"present\":%s,\"type\":\"%s\",\"capacity_mb\":%llu,"
             "\"owner\":\"%s\",\"fs_mounted\":%s,"
             "\"usb_io\":{\"active\":%s,\"read_bytes\":%llu,\"write_bytes\":%llu,"
             "\"ms_since_read\":%u,\"ms_since_write\":%u},"
             "\"web_io\":{\"active\":%s,\"operations\":%lu,"
             "\"read_bytes\":%llu,\"write_bytes\":%llu,\"ms_since_io\":%u}},"
             "\"hid\":{\"host_ready\":%s,\"last_activity_ms\":%u,"
             "\"queued\":%lu,\"sent_reports\":%lu,\"dropped\":%lu,\"queue_depth\":%lu}"
             "}",
             usb_gadget_func_active(USB_FUNC_ACM) ? "true" : "false",
             usb_gadget_func_active(USB_FUNC_MSC) ? "true" : "false",
             usb_gadget_func_active(USB_FUNC_HID) ? "true" : "false",
             s_storage_share_enabled ? "true" : "false",
             s_server ? "true" : "false",
             bridge_nat_mode() ? "nat" : "l2",
             wifi_state_name(wifi.state), wifi.ssid, wifi.rssi,
             sta_ip, usb_ip,
             (unsigned long long)bridge.bytes_to_host,
             (unsigned long long)bridge.bytes_to_wifi,
             (double)bridge.rate_to_host_bps, (double)bridge.rate_to_wifi_bps,
             (unsigned long)bridge.frames_to_host,
             (unsigned long)bridge.frames_to_wifi,
             (unsigned long)bridge.drop_tx,
             (unsigned long)bridge.drop_rx,
             (unsigned long)bridge.drop_refl,
             sdcard_present() ? "true" : "false",
             sdcard_type_str(),
             (unsigned long long)(sdcard_capacity_bytes() / (1024ULL * 1024ULL)),
             owner,
             sdcard_fs_mounted() ? "true" : "false",
             io.active ? "true" : "false",
             (unsigned long long)io.read_bytes, (unsigned long long)io.write_bytes,
             (unsigned)io.ms_since_read, (unsigned)io.ms_since_write,
             web_active ? "true" : "false", (unsigned long)s_web_ops,
             (unsigned long long)s_web_read_bytes,
             (unsigned long long)s_web_write_bytes, (unsigned)web_ms,
             hid_host_ready() ? "true" : "false",
             (unsigned)hid_ms_since_activity(),
             (unsigned long)hid_stats.queued_events,
             (unsigned long)hid_stats.sent_reports,
             (unsigned long)hid_stats.dropped_events,
             (unsigned long)hid_stats.queue_depth);
    return send_json(req, "200 OK", buf);
}

/* Force the on-device share to take the card from the USB host. */
static esp_err_t api_sd_takeover_post(httpd_req_t *req)
{
    if (!s_storage_share_enabled) {
        return send_json(req, "409 Conflict", "{\"error\":\"share_disabled\"}");
    }
    if (!sdcard_present()) {
        return send_json(req, "409 Conflict", "{\"error\":\"no_card\"}");
    }
    esp_err_t err = sdcard_take_esp();
    if (err != ESP_OK) {
        return send_json(req, "500 Internal Server Error", "{\"error\":\"mount_failed\"}");
    }
    return send_json(req, "200 OK", "{\"ok\":true,\"owner\":\"esp\"}");
}

/* Release the card back to the USB host. */
static esp_err_t api_sd_release_post(httpd_req_t *req)
{
    if (!usb_gadget_func_active(USB_FUNC_MSC)) {
        return send_json(req, "409 Conflict",
                         "{\"error\":\"msc_disabled\",\"msg\":\"USB storage mode is not enabled\"}");
    }
    if (s_web_ops > 0) {
        return send_json(req, "409 Conflict",
                         "{\"error\":\"web_busy\",\"msg\":\"A web file operation is active\"}");
    }
    sdcard_release_to_host();
    return send_json(req, "200 OK", "{\"ok\":true,\"owner\":\"host\"}");
}

/* ------------------------------------------------------------------ */
/* File manager JSON API                                              */
/* ------------------------------------------------------------------ */

static esp_err_t api_list_get(httpd_req_t *req)
{
    if (!sd_ready()) {
        return send_sd_busy(req);
    }
    char rel[MAX_PATH_SZ] = "/";
    get_query(req, "path", rel, sizeof(rel));
    char fs[MAX_PATH_SZ];
    if (!build_fs_path(fs, sizeof(fs), rel)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"bad_path\"}");
    }

    DIR *dir = opendir(fs);
    if (!dir) {
        return send_json(req, "404 Not Found", "{\"error\":\"not_found\"}");
    }
    web_op_begin();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr_chunk(req, "{\"path\":\"");
    httpd_resp_sendstr_chunk(req, rel);
    httpd_resp_sendstr_chunk(req, "\",\"entries\":[");

    struct dirent *de;
    bool first = true;
    char item[MAX_PATH_SZ + 128];
    while ((de = readdir(dir)) != NULL) {
        char child[MAX_PATH_SZ];
        snprintf(child, sizeof(child), "%s/%s", fs, de->d_name);
        struct stat st;
        long size = 0;
        bool is_dir = (de->d_type == DT_DIR);
        if (stat(child, &st) == 0) {
            size = (long)st.st_size;
            is_dir = S_ISDIR(st.st_mode);
        }
        snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%ld}",
                 first ? "" : ",", de->d_name, is_dir ? "true" : "false", size);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    closedir(dir);
    web_op_end();
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t api_download_get(httpd_req_t *req)
{
    if (!sd_ready()) {
        return send_sd_busy(req);
    }
    char rel[MAX_PATH_SZ];
    if (!get_query(req, "path", rel, sizeof(rel))) {
        return send_json(req, "400 Bad Request", "{\"error\":\"no_path\"}");
    }
    char fs[MAX_PATH_SZ];
    if (!build_fs_path(fs, sizeof(fs), rel)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"bad_path\"}");
    }
    FILE *f = fopen(fs, "rb");
    if (!f) {
        return send_json(req, "404 Not Found", "{\"error\":\"not_found\"}");
    }
    web_op_begin();
    httpd_resp_set_type(req, "application/octet-stream");
    const char *base = strrchr(fs, '/');
    char disp[MAX_PATH_SZ];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", base ? base + 1 : "file");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char *buf = malloc(XFER_BUF_SZ);
    if (!buf) {
        fclose(f);
        web_op_end();
        return ESP_ERR_NO_MEM;
    }
    size_t n;
    while ((n = fread(buf, 1, XFER_BUF_SZ, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            break;
        }
        web_io_add(false, n);
    }
    free(buf);
    fclose(f);
    web_op_end();
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Streaming body -> file. Returns bytes written or -1. */
static int recv_body_to_file(httpd_req_t *req, const char *fs_path)
{
    FILE *f = fopen(fs_path, "wb");
    if (!f) {
        return -1;
    }
    web_op_begin();
    char *buf = malloc(XFER_BUF_SZ);
    if (!buf) {
        fclose(f);
        web_op_end();
        return -1;
    }
    int remaining = req->content_len;
    int total = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < XFER_BUF_SZ ? remaining : XFER_BUF_SZ);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            fclose(f);
            web_op_end();
            return -1;
        }
        fwrite(buf, 1, r, f);
        web_io_add(true, (size_t)r);
        remaining -= r;
        total += r;
    }
    free(buf);
    fclose(f);
    web_op_end();
    return total;
}

static esp_err_t api_upload_post(httpd_req_t *req)
{
    if (!sd_ready()) {
        return send_sd_busy(req);
    }
    char rel[MAX_PATH_SZ];
    if (!get_query(req, "path", rel, sizeof(rel))) {
        return send_json(req, "400 Bad Request", "{\"error\":\"no_path\"}");
    }
    char fs[MAX_PATH_SZ];
    if (!build_fs_path(fs, sizeof(fs), rel)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"bad_path\"}");
    }
    int written = recv_body_to_file(req, fs);
    if (written < 0) {
        return send_json(req, "500 Internal Server Error",
                         "{\"error\":\"write_failed\",\"msg\":\"Upload write failed\"}");
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"bytes\":%d}", written);
    return send_json(req, "200 OK", buf);
}

static esp_err_t api_delete_post(httpd_req_t *req)
{
    if (!sd_ready()) {
        return send_sd_busy(req);
    }
    char rel[MAX_PATH_SZ];
    if (!get_query(req, "path", rel, sizeof(rel))) {
        return send_json(req, "400 Bad Request", "{\"error\":\"no_path\"}");
    }
    char fs[MAX_PATH_SZ];
    if (!build_fs_path(fs, sizeof(fs), rel)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"bad_path\"}");
    }
    struct stat st;
    int rc = -1;
    if (stat(fs, &st) == 0) {
        rc = S_ISDIR(st.st_mode) ? rmdir(fs) : unlink(fs);
    }
    return send_json(req, rc == 0 ? "200 OK" : "500 Internal Server Error",
                     rc == 0 ? "{\"ok\":true}" : "{\"error\":\"delete_failed\"}");
}

static esp_err_t api_mkdir_post(httpd_req_t *req)
{
    if (!sd_ready()) {
        return send_sd_busy(req);
    }
    char rel[MAX_PATH_SZ];
    if (!get_query(req, "path", rel, sizeof(rel))) {
        return send_json(req, "400 Bad Request", "{\"error\":\"no_path\"}");
    }
    char fs[MAX_PATH_SZ];
    if (!build_fs_path(fs, sizeof(fs), rel)) {
        return send_json(req, "400 Bad Request", "{\"error\":\"bad_path\"}");
    }
    int rc = mkdir(fs, 0775);
    return send_json(req, rc == 0 ? "200 OK" : "500 Internal Server Error",
                     rc == 0 ? "{\"ok\":true}" : "{\"error\":\"mkdir_failed\"}");
}

/* ------------------------------------------------------------------ */
/* HID remote control endpoints                                       */
/* ------------------------------------------------------------------ */

/* Returns true when HID events may be queued. On failure, already sent JSON. */
static bool hid_check_ready(httpd_req_t *req)
{
    if (!usb_gadget_func_active(USB_FUNC_HID)) {
        send_json(req, "409 Conflict",
                  "{\"error\":\"hid_disabled\",\"msg\":\"HID mode is disabled\"}");
        return false;
    }
    /* Only require USB mount — not tud_hid_ready(), which flickers false
     * whenever the endpoint buffer is busy and would break mouse streaming. */
    if (!hid_host_ready()) {
        send_json(req, "409 Conflict",
                  "{\"error\":\"host_not_ready\",\"msg\":\"USB HID host is not ready\"}");
        return false;
    }
    return true;
}

static esp_err_t hid_queue_response(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_OK) {
        return send_json(req, "200 OK", "{\"ok\":true}");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json(req, "503 Service Unavailable",
                         "{\"error\":\"hid_unavailable\",\"msg\":\"HID queue is not ready\"}");
    }
    return send_json(req, "503 Service Unavailable",
                     "{\"error\":\"queue_full\",\"msg\":\"HID event queue is full\"}");
}

static esp_err_t api_hid_text_post(httpd_req_t *req)
{
    if (!hid_check_ready(req)) {
        return ESP_OK;
    }
    char body[256];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    if (len <= 0) {
        return send_json(req, "400 Bad Request",
                         "{\"error\":\"no_body\",\"msg\":\"No text to type\"}");
    }
    int r = httpd_req_recv(req, body, len);
    if (r <= 0) {
        return send_json(req, "400 Bad Request",
                         "{\"error\":\"no_body\",\"msg\":\"Failed to read text body\"}");
    }
    body[r] = '\0';
    char enter[4] = "0";
    get_query(req, "enter", enter, sizeof(enter));
    return hid_queue_response(req, hid_queue_text(body, enter[0] == '1'));
}

static esp_err_t api_hid_key_post(httpd_req_t *req)
{
    if (!hid_check_ready(req)) {
        return ESP_OK;
    }
    char mods[8] = "0", kc[8] = "0";
    get_query(req, "mod", mods, sizeof(mods));
    get_query(req, "kc", kc, sizeof(kc));
    return hid_queue_response(req,
                              hid_queue_key((uint8_t)atoi(mods), (uint8_t)atoi(kc)));
}

static esp_err_t api_hid_mouse_post(httpd_req_t *req)
{
    if (!hid_check_ready(req)) {
        return ESP_OK;
    }
    char dx[8] = "0", dy[8] = "0", btn[8] = "0", wh[8] = "0";
    get_query(req, "dx", dx, sizeof(dx));
    get_query(req, "dy", dy, sizeof(dy));
    get_query(req, "btn", btn, sizeof(btn));
    get_query(req, "wheel", wh, sizeof(wh));
    return hid_queue_response(req,
                              hid_queue_mouse((uint8_t)atoi(btn), atoi(dx), atoi(dy), atoi(wh)));
}

static esp_err_t api_hid_click_post(httpd_req_t *req)
{
    if (!hid_check_ready(req)) {
        return ESP_OK;
    }
    return hid_queue_response(req, hid_queue_left_click());
}

/* ------------------------------------------------------------------ */
/* WebDAV                                                             */
/* ------------------------------------------------------------------ */

/* Extract the decoded path under /dav from the request URI. */
static bool dav_rel_path(httpd_req_t *req, char *out, size_t out_sz)
{
    const char *uri = req->uri;
    const char *p = strstr(uri, "/dav");
    if (!p) {
        return false;
    }
    p += 4; /* skip "/dav" */
    /* strip query string */
    char tmp[MAX_PATH_SZ];
    size_t i = 0;
    while (p[i] && p[i] != '?' && i < sizeof(tmp) - 1) {
        tmp[i] = p[i];
        i++;
    }
    tmp[i] = '\0';
    url_decode(tmp, out, out_sz);
    if (out[0] == '\0') {
        strncpy(out, "/", out_sz);
    }
    return true;
}

static esp_err_t dav_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "DAV", "1, 2");
    httpd_resp_set_hdr(req, "Allow",
                       "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, MKCOL, MOVE, COPY");
    httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, NULL, 0);
}

static void dav_send_prop(httpd_req_t *req, const char *href, bool is_dir, long size)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
             "<D:response><D:href>%s</D:href><D:propstat><D:prop>"
             "<D:resourcetype>%s</D:resourcetype>"
             "<D:getcontentlength>%ld</D:getcontentlength>"
             "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>",
             href, is_dir ? "<D:collection/>" : "", size);
    httpd_resp_sendstr_chunk(req, buf);
}

static esp_err_t dav_propfind(httpd_req_t *req)
{
    if (!sd_ready()) {
        return send_sd_busy(req);
    }
    char rel[MAX_PATH_SZ];
    if (!dav_rel_path(req, rel, sizeof(rel))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    char fs[MAX_PATH_SZ];
    if (!build_fs_path(fs, sizeof(fs), rel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    struct stat st;
    if (stat(fs, &st) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }

    char depth[8] = "0";
    httpd_req_get_hdr_value_str(req, "Depth", depth, sizeof(depth));

    httpd_resp_set_status(req, "207 Multi-Status");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    httpd_resp_sendstr_chunk(req, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                  "<D:multistatus xmlns:D=\"DAV:\">");

    char href[MAX_PATH_SZ];
    snprintf(href, sizeof(href), "/dav%s", rel);
    dav_send_prop(req, href, S_ISDIR(st.st_mode), (long)st.st_size);

    if (S_ISDIR(st.st_mode) && depth[0] != '0') {
        DIR *dir = opendir(fs);
        if (dir) {
            struct dirent *de;
            while ((de = readdir(dir)) != NULL) {
                char child_fs[MAX_PATH_SZ];
                snprintf(child_fs, sizeof(child_fs), "%s/%s", fs, de->d_name);
                struct stat cst;
                bool cdir = (de->d_type == DT_DIR);
                long csize = 0;
                if (stat(child_fs, &cst) == 0) {
                    cdir = S_ISDIR(cst.st_mode);
                    csize = (long)cst.st_size;
                }
                char child_href[MAX_PATH_SZ];
                snprintf(child_href, sizeof(child_href), "/dav%s%s%s", rel,
                         (rel[strlen(rel) - 1] == '/') ? "" : "/", de->d_name);
                dav_send_prop(req, child_href, cdir, csize);
            }
            closedir(dir);
        }
    }

    httpd_resp_sendstr_chunk(req, "</D:multistatus>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t dav_get(httpd_req_t *req)
{
    if (!sd_ready()) {
        return send_sd_busy(req);
    }
    char rel[MAX_PATH_SZ], fs[MAX_PATH_SZ];
    if (!dav_rel_path(req, rel, sizeof(rel)) || !build_fs_path(fs, sizeof(fs), rel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    FILE *f = fopen(fs, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    if (req->method == HTTP_HEAD) {
        fclose(f);
        return httpd_resp_send(req, NULL, 0);
    }
    web_op_begin();
    char *buf = malloc(XFER_BUF_SZ);
    if (!buf) {
        fclose(f);
        web_op_end();
        return ESP_ERR_NO_MEM;
    }
    size_t n;
    while ((n = fread(buf, 1, XFER_BUF_SZ, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            break;
        }
        web_io_add(false, n);
    }
    free(buf);
    fclose(f);
    web_op_end();
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t dav_put(httpd_req_t *req)
{
    if (!sd_ready()) {
        return send_sd_busy(req);
    }
    char rel[MAX_PATH_SZ], fs[MAX_PATH_SZ];
    if (!dav_rel_path(req, rel, sizeof(rel)) || !build_fs_path(fs, sizeof(fs), rel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (recv_body_to_file(req, fs) < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }
    httpd_resp_set_status(req, "201 Created");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dav_delete(httpd_req_t *req)
{
    if (!sd_ready()) {
        return send_sd_busy(req);
    }
    char rel[MAX_PATH_SZ], fs[MAX_PATH_SZ];
    if (!dav_rel_path(req, rel, sizeof(rel)) || !build_fs_path(fs, sizeof(fs), rel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    struct stat st;
    int rc = -1;
    if (stat(fs, &st) == 0) {
        rc = S_ISDIR(st.st_mode) ? rmdir(fs) : unlink(fs);
    }
    httpd_resp_set_status(req, rc == 0 ? "204 No Content" : "404 Not Found");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dav_mkcol(httpd_req_t *req)
{
    if (!sd_ready()) {
        return send_sd_busy(req);
    }
    char rel[MAX_PATH_SZ], fs[MAX_PATH_SZ];
    if (!dav_rel_path(req, rel, sizeof(rel)) || !build_fs_path(fs, sizeof(fs), rel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    int rc = mkdir(fs, 0775);
    httpd_resp_set_status(req, rc == 0 ? "201 Created" : "405 Method Not Allowed");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dav_move(httpd_req_t *req)
{
    if (!sd_ready()) {
        return send_sd_busy(req);
    }
    char rel[MAX_PATH_SZ], fs[MAX_PATH_SZ];
    if (!dav_rel_path(req, rel, sizeof(rel)) || !build_fs_path(fs, sizeof(fs), rel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    char dest_hdr[MAX_PATH_SZ];
    if (httpd_req_get_hdr_value_str(req, "Destination", dest_hdr, sizeof(dest_hdr)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no destination");
    }
    /* Destination is an absolute URL; take the path after /dav. */
    char *dpath = strstr(dest_hdr, "/dav");
    if (!dpath) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad destination");
    }
    dpath += 4;
    char drel[MAX_PATH_SZ], dfs[MAX_PATH_SZ];
    url_decode(dpath, drel, sizeof(drel));
    if (!build_fs_path(dfs, sizeof(dfs), drel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad destination");
    }
    int rc = rename(fs, dfs);
    httpd_resp_set_status(req, rc == 0 ? "201 Created" : "500 Internal Server Error");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t dav_dispatch(httpd_req_t *req)
{
    switch (req->method) {
    case HTTP_OPTIONS:  return dav_options(req);
    case HTTP_PROPFIND: return dav_propfind(req);
    case HTTP_GET:
    case HTTP_HEAD:     return dav_get(req);
    case HTTP_PUT:      return dav_put(req);
    case HTTP_DELETE:   return dav_delete(req);
    case HTTP_MKCOL:    return dav_mkcol(req);
    case HTTP_MOVE:     return dav_move(req);
    default:
        httpd_resp_set_status(req, "405 Method Not Allowed");
        return httpd_resp_send(req, NULL, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Web UI                                                             */
/* ------------------------------------------------------------------ */

static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>T-Dongle-S3</title><style>"
"body{font-family:system-ui,sans-serif;margin:0;background:#0f1720;color:#e6edf3}"
"header{background:#161b22;padding:10px 16px;font-weight:600}"
"main{padding:16px;max-width:900px;margin:0 auto}"
".card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:10px 0}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:10px}"
".grid .card{margin:0}.kv{display:grid;grid-template-columns:auto 1fr;gap:3px 10px;font-size:13px}"
".kv span:nth-child(odd){color:#8b949e}.ok{color:#3fb950}.bad{color:#f85149}"
".busy{background:#3d2b0f;border-color:#9e6a00}"
"button{background:#238636;color:#fff;border:0;border-radius:6px;padding:8px 12px;cursor:pointer;margin:2px}"
"button.warn{background:#9e6a00}button.d{background:#6e2630}"
"button:disabled{opacity:.5;cursor:not-allowed}"
"a{color:#58a6ff;text-decoration:none}"
"table{width:100%;border-collapse:collapse}td{padding:6px;border-bottom:1px solid #30363d}"
".r{text-align:right}.mut{color:#8b949e;font-size:12px}"
"input[type=text]{background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:6px;padding:8px;width:60%}"
"#pad{width:100%;height:140px;background:#0d1117;border:1px dashed #30363d;border-radius:8px;touch-action:none;display:grid;place-items:center;color:#8b949e;user-select:none}"
"#xferbar{display:none;height:8px;background:#0d1117;border:1px solid #30363d;border-radius:4px;margin-top:8px;overflow:hidden}"
"#xferfill{height:100%;width:0;background:#238636;transition:width .1s linear}"
"</style></head><body>"
"<header>T-Dongle-S3 control</header><main>"
"<div class=grid>"
"<div class=card><b>Device state</b><div class=kv id=device></div></div>"
"<div class=card><b>Network</b><div class=kv id=network></div></div>"
"<div class=card><b>USB modes</b><div class=kv id=modes></div></div>"
"<div class=card><b>Traffic</b><div class=kv id=traffic></div></div>"
"</div>"
"<div id=sdbanner></div>"
"<div class=card id=storagecard><b>Storage</b> <span id=sdinfo class=mut></span><div id=io class=mut></div>"
"<div id=storageControls style=margin-top:8px></div></div>"
"<div class=card id=filecard><div id=crumbs></div><table id=files></table>"
"<div style=margin-top:8px><input type=text id=newdir placeholder=\"new folder\">"
"<button onclick=mkdir()>mkdir</button></div>"
"<div style=margin-top:8px><input type=file id=fu>"
"<button id=upbtn onclick=upload()>Upload</button></div>"
"<div id=xfer class=mut></div><div id=xferbar><div id=xferfill></div></div></div>"
"<div class=card id=hidcard style=\"display:none\"><b>HID remote</b>"
"<div id=hidstate class=mut></div>"
"<form id=hidform style=margin-top:8px><input type=text id=hidtext placeholder=\"Text to type on USB host\">"
"<label><input type=checkbox id=hidenter> Press Enter afterward</label>"
"<button type=submit>Type text</button></form>"
"<div><span class=mut>Special keys:</span> <button onclick=hidKey(0,43)>Tab</button></div>"
"<div id=pad>Drag to move &bull; tap to left-click</div><div id=hidfeedback class=mut></div></div>"
"<div class=mut id=davline>WebDAV: <span id=davurl></span>/dav/</div>"
"</main><script>"
"let cwd='/',lastOwner='',listed=false,current=null;"
"async function j(u,o){let r=await fetch(u,o),d=await r.json().catch(()=>({error:'bad_response'}));if(!r.ok)throw d;return d}"
"function fmt(b){b=+b;if(b<1024)return b+' B';if(b<1048576)return (b/1024).toFixed(1)+' KB';return (b/1048576).toFixed(1)+' MB'}"
"function kv(id,a){document.getElementById(id).innerHTML=a.map(x=>'<span>'+x[0]+'</span><span>'+x[1]+'</span>').join('')}"
"async function status(){try{let s=await j('/api/status');current=s;"
"kv('device',[['Wi-Fi',s.wifi.state],['SSID',s.wifi.ssid||'—'],['SD',s.sd.present?s.sd.type+' '+s.sd.capacity_mb+' MB':'not detected'],['SD owner',s.sd.owner]]);"
"kv('network',[['Mode',s.modes.network.toUpperCase()],['STA IP',s.wifi.sta_ip||'—'],['USB IP',s.wifi.usb_ip||'—'],['RSSI',s.wifi.rssi+' dBm']]);"
"kv('modes',[['NCM','ON'],['ACM',s.modes.acm?'ON':'OFF'],['USB storage',s.modes.msc?'ON':'OFF'],['HID',s.modes.hid?'ON':'OFF'],['WebDAV/share',s.modes.share?'ON':'OFF']]);"
"kv('traffic',[['Download',fmt(s.bridge.down_bytes)+' / '+fmt(s.bridge.down_bps)+'/s'],['Upload',fmt(s.bridge.up_bytes)+' / '+fmt(s.bridge.up_bps)+'/s'],['Frames',s.bridge.frames_to_host+' / '+s.bridge.frames_to_wifi],['Drops',s.bridge.drop_tx+' / '+s.bridge.drop_rx]]);"
"let share=s.modes.share;document.getElementById('storagecard').style.display=share?'block':'none';"
"document.getElementById('filecard').style.display=share&&s.sd.owner==='esp'?'block':'none';"
"document.getElementById('davline').style.display=share?'block':'none';"
"document.getElementById('hidcard').style.display=s.modes.hid?'block':'none';"
"document.getElementById('hidstate').textContent=s.modes.hid?(s.hid.host_ready?'USB host ready':'Waiting for USB host'):'disabled';"
"if(share){document.getElementById('sdinfo').textContent=s.sd.present?(s.sd.type+' '+s.sd.capacity_mb+' MB, owner='+s.sd.owner):'no card';"
"document.getElementById('io').textContent='USB reads '+fmt(s.sd.usb_io.read_bytes)+' / writes '+fmt(s.sd.usb_io.write_bytes)+(s.sd.usb_io.active?' — ACTIVE':' — idle')+"
"' · web writes '+fmt(s.sd.web_io.write_bytes)+' / reads '+fmt(s.sd.web_io.read_bytes)+(s.sd.web_io.active?' — ACTIVE':' — idle');"
"let b=document.getElementById('sdbanner'),c=document.getElementById('storageControls');b.className='';b.innerHTML='';c.innerHTML='';"
"if(s.sd.present&&s.sd.owner!=='esp'){b.className='card busy';b.innerHTML='<b>SD is assigned to USB storage.</b> '+(s.sd.usb_io.active?'<span class=bad>USB transfer active.</span> ':'')+'<button class=warn onclick=takeover('+(s.sd.usb_io.active?'1':'0')+')>Use SD in web UI</button>';}"
"else if(s.sd.owner==='esp'&&s.modes.msc){c.innerHTML='<button class=warn onclick=releaseToUsb('+(s.sd.web_io.active?'1':'0')+')>Give SD to USB</button>';}"
"else if(s.sd.owner==='esp'&&!s.modes.msc){c.innerHTML='<span class=mut>Enable USB storage mode on the device before assigning the SD to USB.</span>';}"
"if(s.sd.owner==='esp'&&(!listed||lastOwner!=='esp')){list(cwd);listed=true;}if(s.sd.owner!=='esp'){document.getElementById('files').innerHTML='';listed=false;}"
"lastOwner=s.sd.owner;}"
"}catch(e){console.error(e)}}"
"async function takeover(active){if(active&&!confirm('USB read/write activity is in progress. Interrupting it may corrupt data. Continue?'))return;try{await j('/api/sd/takeover',{method:'POST'});listed=false;await status()}catch(e){alert(e.msg||e.error)}}"
"async function releaseToUsb(active){if(active&&!confirm('A web file operation was recently active. Give the SD to USB now?'))return;try{await j('/api/sd/release',{method:'POST'});document.getElementById('files').innerHTML='';listed=false;await status()}catch(e){alert(e.msg||e.error)}}"
"function list(p){cwd=p;j('/api/list?path='+encodeURIComponent(p)).then(d=>{"
"if(d.error){return}let t=document.getElementById('files');t.innerHTML='';"
"document.getElementById('crumbs').innerHTML='<b>'+p+'</b> '+(p!=='/'?'<a href=# onclick=\"list(up())\">[up]</a>':'');"
"d.entries.sort((a,b)=>b.dir-a.dir).forEach(e=>{let tr=document.createElement('tr');"
"let np=(p==='/'?'':p)+'/'+e.name;"
"tr.innerHTML=(e.dir?'<td>&#128193; <a href=# onclick=\"list(\\''+np+'\\')\">'+e.name+'</a></td><td class=r></td>':"
"'<td>&#128196; <a href=\"/dl?path='+encodeURIComponent(np)+'\">'+e.name+'</a></td><td class=r>'+fmt(e.size)+'</td>')"
"+'<td class=r><button class=d onclick=\"del(\\''+np+'\\')\">del</button></td>';t.appendChild(tr);});});}"
"function up(){let x=cwd.replace(/\\/[^/]*$/,'');return x||'/'}"
"function del(p){if(!confirm('Delete '+p+'?'))return;fetch('/api/delete?path='+encodeURIComponent(p),{method:'POST'}).then(()=>list(cwd));}"
"function mkdir(){let n=document.getElementById('newdir').value;if(!n)return;let np=(cwd==='/'?'':cwd)+'/'+n;"
"fetch('/api/mkdir?path='+encodeURIComponent(np),{method:'POST'}).then(()=>list(cwd));}"
"function setXfer(t,bad,pct){let e=document.getElementById('xfer');e.textContent=t;e.className=bad?'bad':(pct>=100?'ok':'mut');"
"let bar=document.getElementById('xferbar'),fill=document.getElementById('xferfill');"
"if(pct==null){bar.style.display='none';fill.style.width='0';return}"
"bar.style.display='block';fill.style.width=Math.max(0,Math.min(100,pct))+'%';fill.style.background=bad?'#f85149':'#238636'}"
"function upload(){let inp=document.getElementById('fu'),btn=document.getElementById('upbtn'),f=inp.files[0];"
"if(!f){setXfer('Choose a file to upload',true,null);return}"
"if(btn.disabled)return;let np=(cwd==='/'?'':cwd)+'/'+f.name;btn.disabled=true;inp.disabled=true;"
"setXfer('Starting upload of '+f.name+' ('+fmt(f.size)+')…',false,0);"
"let xhr=new XMLHttpRequest();xhr.open('POST','/api/upload?path='+encodeURIComponent(np));"
"xhr.upload.onprogress=e=>{if(e.lengthComputable&&e.total>0){let pct=Math.round(100*e.loaded/e.total);"
"setXfer('Uploading '+f.name+': '+fmt(e.loaded)+' / '+fmt(e.total)+' ('+pct+'%)',false,pct)}"
"else{setXfer('Uploading '+f.name+': '+fmt(e.loaded)+'…',false,null)}};"
"xhr.onload=()=>{btn.disabled=false;inp.disabled=false;let ok=xhr.status>=200&&xhr.status<300,msg='';"
"try{let d=JSON.parse(xhr.responseText||'{}');msg=d.msg||d.error||'';if(ok&&d.bytes!=null)msg='Uploaded '+f.name+' ('+fmt(d.bytes)+')'}"
"catch(x){}if(ok){if(!msg)msg='Uploaded '+f.name+' ('+fmt(f.size)+')';setXfer(msg,false,100);inp.value='';list(cwd)}"
"else{setXfer(msg||('Upload failed (HTTP '+xhr.status+')'),true,100)}};"
"xhr.onerror=()=>{btn.disabled=false;inp.disabled=false;setXfer('Upload failed (network error)',true,100)};"
"xhr.ontimeout=()=>{btn.disabled=false;inp.disabled=false;setXfer('Upload timed out',true,100)};"
"xhr.send(f)}"
"document.getElementById('fu').addEventListener('change',e=>{let f=e.target.files&&e.target.files[0];"
"if(f)setXfer('Selected '+f.name+' ('+fmt(f.size)+') — click Upload',false,null);else setXfer('',false,null)});"
"function feedback(t,bad){let e=document.getElementById('hidfeedback');e.textContent=t;e.className=bad?'bad':'mut'}"
"async function hidType(){let t=document.getElementById('hidtext').value;if(!t)return;try{await j('/api/hid/text?enter='+(document.getElementById('hidenter').checked?'1':'0'),{method:'POST',body:t});feedback('Text queued',false)}catch(e){feedback(e.msg||e.error,true)}}"
"async function hidKey(m,k){try{await j('/api/hid/key?mod='+m+'&kc='+k,{method:'POST'});feedback('Key queued',false)}catch(e){feedback(e.msg||e.error,true)}}"
"document.getElementById('hidform').addEventListener('submit',e=>{e.preventDefault();hidType()});"
"let pad=document.getElementById('pad'),lx=0,ly=0,sx=0,sy=0,started=0,down=false,moved=0;"
"pad.addEventListener('pointerdown',e=>{down=true;lx=sx=e.clientX;ly=sy=e.clientY;started=Date.now();moved=0;pad.setPointerCapture(e.pointerId);});"
"pad.addEventListener('pointerup',async e=>{if(!down)return;down=false;if(moved<6&&Date.now()-started<600){try{await j('/api/hid/click',{method:'POST'});feedback('Left click',false)}catch(x){feedback(x.msg||x.error,true)}}});"
"pad.addEventListener('pointercancel',e=>{down=false});"
"pad.addEventListener('pointermove',e=>{if(!down)return;let dx=e.clientX-lx,dy=e.clientY-ly;lx=e.clientX;ly=e.clientY;moved=Math.max(moved,Math.hypot(e.clientX-sx,e.clientY-sy));"
"if(Math.abs(dx)+Math.abs(dy)>1)fetch('/api/hid/mouse?dx='+Math.round(dx)+'&dy='+Math.round(dy),{method:'POST'});});"
"document.getElementById('davurl').textContent='http://'+location.host;"
"status();setInterval(status,2000);"
"</script></body></html>";

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

static void reg(httpd_handle_t s, const char *uri, httpd_method_t m, esp_err_t (*h)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = m, .handler = h };
    httpd_register_uri_handler(s, &u);
}

esp_err_t httpd_share_start(bool storage_share_enabled)
{
    if (s_server) {
        s_storage_share_enabled = storage_share_enabled;
        return ESP_OK;
    }
    s_storage_share_enabled = storage_share_enabled;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 24;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.server_port = 80;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = NULL;
        return ESP_FAIL;
    }

    reg(s_server, "/", HTTP_GET, index_get);
    reg(s_server, "/api/status", HTTP_GET, api_status_get);
    reg(s_server, "/api/sd/takeover", HTTP_POST, api_sd_takeover_post);
    reg(s_server, "/api/sd/release", HTTP_POST, api_sd_release_post);
    reg(s_server, "/api/list", HTTP_GET, api_list_get);
    reg(s_server, "/dl", HTTP_GET, api_download_get);
    reg(s_server, "/api/upload", HTTP_POST, api_upload_post);
    reg(s_server, "/api/delete", HTTP_POST, api_delete_post);
    reg(s_server, "/api/mkdir", HTTP_POST, api_mkdir_post);
    reg(s_server, "/api/hid/text", HTTP_POST, api_hid_text_post);
    reg(s_server, "/api/hid/key", HTTP_POST, api_hid_key_post);
    reg(s_server, "/api/hid/mouse", HTTP_POST, api_hid_mouse_post);
    reg(s_server, "/api/hid/click", HTTP_POST, api_hid_click_post);

    /* WebDAV: one wildcard route per supported method. */
    const httpd_method_t dav_methods[] = {
        HTTP_OPTIONS, HTTP_PROPFIND, HTTP_GET, HTTP_HEAD, HTTP_PUT,
        HTTP_DELETE, HTTP_MKCOL, HTTP_MOVE,
    };
    for (size_t i = 0; i < sizeof(dav_methods) / sizeof(dav_methods[0]); i++) {
        reg(s_server, "/dav*", dav_methods[i], dav_dispatch);
    }

    ESP_LOGI(TAG, "SD share web server started on :80");
    return ESP_OK;
}

void httpd_share_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

bool httpd_share_running(void)
{
    return s_server != NULL;
}

bool httpd_share_storage_enabled(void)
{
    return s_server != NULL && s_storage_share_enabled;
}
