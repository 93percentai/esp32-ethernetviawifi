#include "display.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bridge.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"
#include "esp_log.h"
#include "font8x8.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid.h"
#include "httpd_share.h"
#include "net_tether.h"
#include "provisioning.h"
#include "sdcard.h"
#include "sdkconfig.h"
#include "usb_gadget.h"
#include "wifi_mgr.h"

#if CONFIG_BRIDGE_LCD_ENABLED

static const char *TAG = "display";

#define COLOR_BG     0x1082
#define COLOR_PANEL  0x2104
#define COLOR_TEXT   0xEF7D
#define COLOR_MUTED  0x8410
#define COLOR_OK     0x05E0
#define COLOR_WARN   0xFE60
#define COLOR_ERR    0xF800
#define COLOR_ACCENT 0x07FF

static esp_lcd_panel_handle_t s_panel;
static uint16_t s_fb[BOARD_LCD_H_RES * BOARD_LCD_V_RES];
static volatile display_overlay_t s_overlay = DISPLAY_OVERLAY_NONE;
static volatile int s_overlay_seconds;
static volatile display_screen_t s_screen = SCREEN_OVERVIEW;
static volatile bool s_hold_active;
static volatile int s_hold_pct;
static volatile bool s_hold_reset_zone;
static char s_msg1[24];
static char s_msg2[24];
static char s_hold_hint[24];

static uint16_t swap565(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

static void fb_clear(uint16_t color)
{
    uint16_t c = swap565(color);
    for (int i = 0; i < BOARD_LCD_H_RES * BOARD_LCD_V_RES; i++) {
        s_fb[i] = c;
    }
}

static void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    uint16_t c = swap565(color);
    for (int row = y; row < y + h && row < BOARD_LCD_V_RES; row++) {
        if (row < 0) continue;
        for (int col = x; col < x + w && col < BOARD_LCD_H_RES; col++) {
            if (col < 0) continue;
            s_fb[row * BOARD_LCD_H_RES + col] = c;
        }
    }
}

static void fb_draw_char(int x, int y, char ch, uint16_t fg, uint16_t bg)
{
    if (ch < 32 || ch > 127) ch = '?';
    const uint8_t *glyph = font8x8_basic[(int)ch - 32];
    uint16_t f = swap565(fg);
    uint16_t b = swap565(bg);
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        int py = y + row;
        if (py < 0 || py >= BOARD_LCD_V_RES) continue;
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px < 0 || px >= BOARD_LCD_H_RES) continue;
            s_fb[py * BOARD_LCD_H_RES + px] = (bits & (1u << col)) ? f : b;
        }
    }
}

static void fb_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg)
{
    while (*text) {
        fb_draw_char(x, y, *text++, fg, bg);
        x += 8;
        if (x > BOARD_LCD_H_RES - 8) break;
    }
}

static void fmt_ip(uint32_t addr, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u",
             (unsigned)(addr & 0xff), (unsigned)((addr >> 8) & 0xff),
             (unsigned)((addr >> 16) & 0xff), (unsigned)((addr >> 24) & 0xff));
}

/* ---- Common header ---- */
static const char *screen_title(display_screen_t s)
{
    switch (s) {
    case SCREEN_OVERVIEW: return "WIFI BRIDGE";
    case SCREEN_NETWORK:  return "NETWORK";
    case SCREEN_SD:       return "SD CARD";
    case SCREEN_SHARE:    return "FILE SHARE";
    case SCREEN_HID:      return "HID REMOTE";
    default:              return "T-DONGLE";
    }
}

static void draw_header(display_screen_t s)
{
    fb_fill_rect(0, 0, BOARD_LCD_H_RES, 12, COLOR_PANEL);
    fb_draw_text(2, 2, screen_title(s), COLOR_ACCENT, COLOR_PANEL);
    /* screen position dots on the right */
    for (int i = 0; i < SCREEN_COUNT; i++) {
        int x = BOARD_LCD_H_RES - (SCREEN_COUNT - i) * 7;
        fb_fill_rect(x, 4, 4, 4, i == (int)s ? COLOR_ACCENT : COLOR_MUTED);
    }
}

static const char *link_label(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_IDLE: return "NO CONFIG";
    case WIFI_MGR_CONNECTING: return "ASSOC";
    case WIFI_MGR_CONNECTED: return "CONNECTED";
    case WIFI_MGR_DISCONNECTED: return "RECONNECT";
    case WIFI_MGR_NO_AP: return "NO AP";
    case WIFI_MGR_BAD_AUTH: return "BAD AUTH";
    case WIFI_MGR_SCANNING: return "SCANNING";
    case WIFI_MGR_PROVISIONING: return "SETUP AP";
    default: return "?";
    }
}

static uint16_t link_color(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_CONNECTED: return COLOR_OK;
    case WIFI_MGR_CONNECTING:
    case WIFI_MGR_SCANNING:
    case WIFI_MGR_PROVISIONING:
    case WIFI_MGR_DISCONNECTED: return COLOR_WARN;
    case WIFI_MGR_BAD_AUTH:
    case WIFI_MGR_NO_AP: return COLOR_ERR;
    default: return COLOR_MUTED;
    }
}

static void mode_hint(void)
{
    fb_draw_text(2, 70, "hold 2s: toggle", COLOR_MUTED, COLOR_BG);
}

/* ---- Screens ---- */

static void screen_overview(void)
{
    wifi_mgr_status_t st;
    bridge_stats_t stats;
    wifi_mgr_get_status(&st);
    bridge_get_stats(&stats);

    char line[32];
    fb_draw_text(2, 16, "LINK", COLOR_MUTED, COLOR_BG);
    fb_draw_text(42, 16, link_label(st.state), link_color(st.state), COLOR_BG);

    const char *ssid = st.ssid[0] ? st.ssid : "(unset)";
    snprintf(line, sizeof(line), "%.14s", ssid);
    fb_draw_text(2, 28, "SSID", COLOR_MUTED, COLOR_BG);
    fb_draw_text(42, 28, line, COLOR_TEXT, COLOR_BG);

    char bytes[12], rate[12];
    bridge_format_bytes(stats.bytes_to_host, bytes, sizeof(bytes));
    bridge_format_rate(stats.rate_to_host_bps, rate, sizeof(rate));
    snprintf(line, sizeof(line), "%.10s %.10s", bytes, rate);
    fb_draw_text(2, 44, "DN", COLOR_OK, COLOR_BG);
    fb_draw_text(26, 44, line, COLOR_TEXT, COLOR_BG);

    bridge_format_bytes(stats.bytes_to_wifi, bytes, sizeof(bytes));
    bridge_format_rate(stats.rate_to_wifi_bps, rate, sizeof(rate));
    snprintf(line, sizeof(line), "%.10s %.10s", bytes, rate);
    fb_draw_text(2, 56, "UP", COLOR_WARN, COLOR_BG);
    fb_draw_text(26, 56, line, COLOR_TEXT, COLOR_BG);
}

static void screen_network(void)
{
    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);
    char line[32];

    fb_draw_text(2, 16, "MODE", COLOR_MUTED, COLOR_BG);
    fb_draw_text(42, 16, st.nat_mode ? "NAT TETHER" : "L2 BRIDGE",
                 st.nat_mode ? COLOR_ACCENT : COLOR_OK, COLOR_BG);

    if (st.nat_mode) {
        fb_draw_text(2, 28, "STA", COLOR_MUTED, COLOR_BG);
        if (st.has_ip) {
            fmt_ip(st.sta_ip, line, sizeof(line));
        } else {
            snprintf(line, sizeof(line), "no IP");
        }
        fb_draw_text(34, 28, line, COLOR_TEXT, COLOR_BG);

        esp_netif_ip_info_t usb;
        net_tether_get_usb_ip(&usb);
        fmt_ip(usb.ip.addr, line, sizeof(line));
        fb_draw_text(2, 40, "USB", COLOR_MUTED, COLOR_BG);
        fb_draw_text(34, 40, line, COLOR_TEXT, COLOR_BG);

        fb_draw_text(2, 52, "NAPT", COLOR_MUTED, COLOR_BG);
        fb_draw_text(42, 52, "on", COLOR_OK, COLOR_BG);
    } else {
        fb_draw_text(2, 30, "Host holds the IP", COLOR_TEXT, COLOR_BG);
        fb_draw_text(2, 42, "on the AP subnet.", COLOR_MUTED, COLOR_BG);
        fb_draw_text(2, 54, "ESP has no IP.", COLOR_MUTED, COLOR_BG);
    }
}

static void screen_sd(void)
{
    char line[32];
    if (!sdcard_present()) {
        fb_draw_text(2, 28, "No SD card", COLOR_WARN, COLOR_BG);
        snprintf(line, sizeof(line), "%.18s", sdcard_last_error());
        fb_draw_text(2, 42, line, COLOR_MUTED, COLOR_BG);
        mode_hint();
        return;
    }
    snprintf(line, sizeof(line), "%s %lluMB", sdcard_type_str(),
             (unsigned long long)(sdcard_capacity_bytes() / (1024ULL * 1024ULL)));
    fb_draw_text(2, 16, line, COLOR_TEXT, COLOR_BG);

    bool msc = usb_gadget_func_active(USB_FUNC_MSC);
    fb_draw_text(2, 28, "USB MSC", COLOR_MUTED, COLOR_BG);
    fb_draw_text(66, 28, msc ? "ON" : "OFF", msc ? COLOR_OK : COLOR_MUTED, COLOR_BG);

    const char *own = "idle";
    uint16_t oc = COLOR_MUTED;
    switch (sdcard_owner()) {
    case SD_OWNER_HOST: own = "USB HOST"; oc = COLOR_WARN; break;
    case SD_OWNER_ESP:  own = "WEB/DAV";  oc = COLOR_OK; break;
    default: break;
    }
    fb_draw_text(2, 40, "OWNER", COLOR_MUTED, COLOR_BG);
    fb_draw_text(52, 40, own, oc, COLOR_BG);

    sd_io_stats_t io;
    sdcard_get_io_stats(&io);
    fb_draw_text(2, 52, "IO", COLOR_MUTED, COLOR_BG);
    fb_draw_text(26, 52, io.active ? "ACTIVE" : "idle",
                 io.active ? COLOR_ERR : COLOR_MUTED, COLOR_BG);
    mode_hint();
}

static void screen_share(void)
{
    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);
    bool on = httpd_share_running();

    fb_draw_text(2, 16, "WEB+DAV", COLOR_MUTED, COLOR_BG);
    fb_draw_text(66, 16, on ? "ON" : "OFF", on ? COLOR_OK : COLOR_MUTED, COLOR_BG);

    char line[32];
    if (on && st.has_ip) {
        fb_draw_text(2, 30, "http://", COLOR_MUTED, COLOR_BG);
        fmt_ip(st.sta_ip, line, sizeof(line));
        fb_draw_text(2, 42, line, COLOR_ACCENT, COLOR_BG);
    } else if (on) {
        fb_draw_text(2, 30, "waiting for IP", COLOR_WARN, COLOR_BG);
    } else {
        fb_draw_text(2, 30, "Enable to serve", COLOR_TEXT, COLOR_BG);
        fb_draw_text(2, 42, "SD on the LAN", COLOR_MUTED, COLOR_BG);
    }
    if (sdcard_owner() == SD_OWNER_HOST) {
        fb_draw_text(2, 54, "SD busy (USB)", COLOR_WARN, COLOR_BG);
    }
    mode_hint();
}

static void screen_hid(void)
{
    bool on = usb_gadget_func_active(USB_FUNC_HID);
    fb_draw_text(2, 16, "HID KBD+MOUSE", COLOR_MUTED, COLOR_BG);
    fb_draw_text(2, 28, "STATE", COLOR_MUTED, COLOR_BG);
    fb_draw_text(52, 28, on ? "ON" : "OFF", on ? COLOR_OK : COLOR_MUTED, COLOR_BG);

    if (on) {
        fb_draw_text(2, 40, "HOST", COLOR_MUTED, COLOR_BG);
        fb_draw_text(42, 40, hid_host_ready() ? "READY" : "waiting",
                     hid_host_ready() ? COLOR_OK : COLOR_WARN, COLOR_BG);
        uint32_t ms = hid_ms_since_activity();
        char line[24];
        if (ms == UINT32_MAX) {
            snprintf(line, sizeof(line), "idle");
        } else {
            snprintf(line, sizeof(line), "%lus ago", (unsigned long)(ms / 1000));
        }
        fb_draw_text(2, 52, "LAST", COLOR_MUTED, COLOR_BG);
        fb_draw_text(42, 52, line, COLOR_TEXT, COLOR_BG);
    }
    mode_hint();
}

static void render_conf(const provisioning_lcd_status_t *prov)
{
    char line[32];
    fb_clear(COLOR_BG);
    fb_fill_rect(0, 0, BOARD_LCD_H_RES, 12, COLOR_PANEL);
    fb_draw_text(2, 2, "SETUP", COLOR_ACCENT, COLOR_PANEL);

    const char *link = "SETUP AP";
    uint16_t lc = COLOR_WARN;
    if (prov->phase == PROV_PHASE_SCANNING) link = "SCANNING";
    else if (prov->phase == PROV_PHASE_TESTING) link = "TESTING";
    else if (prov->phase == PROV_PHASE_SUCCESS) { link = "SAVED"; lc = COLOR_OK; }

    fb_draw_text(2, 16, "LINK", COLOR_MUTED, COLOR_BG);
    fb_draw_text(42, 16, link, lc, COLOR_BG);
    fb_draw_text(2, 28, "AP", COLOR_MUTED, COLOR_BG);
    snprintf(line, sizeof(line), "%.17s", PROV_SOFTAP_SSID);
    fb_draw_text(26, 28, line, COLOR_TEXT, COLOR_BG);
    fb_draw_text(2, 40, "URL", COLOR_MUTED, COLOR_BG);
    snprintf(line, sizeof(line), "%d.%d.%d.%d",
             PROV_SOFTAP_IP_A, PROV_SOFTAP_IP_B, PROV_SOFTAP_IP_C, PROV_SOFTAP_IP_D);
    fb_draw_text(42, 40, line, COLOR_ACCENT, COLOR_BG);
    fb_draw_text(2, 52, "NET", COLOR_MUTED, COLOR_BG);
    snprintf(line, sizeof(line), "%d found", prov->scan_count);
    fb_draw_text(42, 52, line, COLOR_TEXT, COLOR_BG);
}

static void render_overlay(void)
{
    char line[24];
    fb_clear(COLOR_BG);
    fb_fill_rect(0, 0, BOARD_LCD_H_RES, 12, COLOR_PANEL);
    fb_draw_text(2, 2, "T-DONGLE", COLOR_ACCENT, COLOR_PANEL);

    if (s_overlay == DISPLAY_OVERLAY_RESET_DONE) {
        fb_draw_text(20, 28, "SETTINGS", COLOR_OK, COLOR_BG);
        fb_draw_text(20, 44, "CLEARED", COLOR_OK, COLOR_BG);
    } else if (s_overlay == DISPLAY_OVERLAY_MODE_APPLIED) {
        fb_draw_text(2, 24, s_msg1, COLOR_ACCENT, COLOR_BG);
        fb_draw_text(2, 40, s_msg2, COLOR_TEXT, COLOR_BG);
        fb_draw_text(2, 60, "rebooting...", COLOR_MUTED, COLOR_BG);
    } else { /* RESET_CONFIRM */
        fb_draw_text(30, 18, "RESET ALL?", COLOR_WARN, COLOR_BG);
        fb_draw_text(4, 34, "Press again in", COLOR_TEXT, COLOR_BG);
        snprintf(line, sizeof(line), "%ds to confirm", s_overlay_seconds);
        fb_draw_text(20, 48, line, COLOR_ACCENT, COLOR_BG);
    }
}

static void draw_hold_bar(void)
{
    if (!s_hold_active) return;
    if (s_hold_hint[0]) {
        /* Clear the hint row then draw it just above the bar. */
        fb_fill_rect(0, BOARD_LCD_V_RES - 18, BOARD_LCD_H_RES, 10, COLOR_BG);
        fb_draw_text(2, BOARD_LCD_V_RES - 17, s_hold_hint,
                     s_hold_reset_zone ? COLOR_ERR : COLOR_ACCENT, COLOR_BG);
    }
    int w = (BOARD_LCD_H_RES - 4) * s_hold_pct / 100;
    if (w < 0) w = 0;
    if (w > BOARD_LCD_H_RES - 4) w = BOARD_LCD_H_RES - 4;
    fb_fill_rect(2, BOARD_LCD_V_RES - 6, BOARD_LCD_H_RES - 4, 4, COLOR_PANEL);
    fb_fill_rect(2, BOARD_LCD_V_RES - 6, w, 4, s_hold_reset_zone ? COLOR_ERR : COLOR_WARN);
}

static void render(void)
{
    if (s_overlay != DISPLAY_OVERLAY_NONE) {
        render_overlay();
        goto flush;
    }

    provisioning_lcd_status_t prov;
    provisioning_get_lcd_status(&prov);
    /* While provisioning, show the setup screen on the overview/network slots
     * but keep the mode screens reachable (e.g. to enable USB-SD without Wi-Fi). */
    if (prov.active && (s_screen == SCREEN_OVERVIEW || s_screen == SCREEN_NETWORK)) {
        render_conf(&prov);
        draw_hold_bar();
        goto flush;
    }

    fb_clear(COLOR_BG);
    draw_header(s_screen);
    switch (s_screen) {
    case SCREEN_OVERVIEW: screen_overview(); break;
    case SCREEN_NETWORK:  screen_network(); break;
    case SCREEN_SD:       screen_sd(); break;
    case SCREEN_SHARE:    screen_share(); break;
    case SCREEN_HID:      screen_hid(); break;
    default: break;
    }
    draw_hold_bar();

flush:
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fb);
}

void display_set_overlay(display_overlay_t kind, int seconds_left)
{
    s_overlay = kind;
    s_overlay_seconds = seconds_left;
}

void display_set_message(const char *line1, const char *line2)
{
    strncpy(s_msg1, line1 ? line1 : "", sizeof(s_msg1) - 1);
    s_msg1[sizeof(s_msg1) - 1] = '\0';
    strncpy(s_msg2, line2 ? line2 : "", sizeof(s_msg2) - 1);
    s_msg2[sizeof(s_msg2) - 1] = '\0';
}

void display_next_screen(void)
{
    s_screen = (display_screen_t)(((int)s_screen + 1) % SCREEN_COUNT);
}

display_screen_t display_current_screen(void)
{
    return s_screen;
}

void display_set_hold(bool active, int pct, bool reset_zone)
{
    s_hold_active = active;
    s_hold_pct = pct;
    s_hold_reset_zone = reset_zone;
    if (!active) {
        s_hold_hint[0] = '\0';
    }
}

void display_set_hold_hint(const char *hint)
{
    strncpy(s_hold_hint, hint ? hint : "", sizeof(s_hold_hint) - 1);
    s_hold_hint[sizeof(s_hold_hint) - 1] = '\0';
}

static void display_task(void *arg)
{
    (void)arg;
    while (true) {
        render();
        vTaskDelay(pdMS_TO_TICKS((s_overlay != DISPLAY_OVERLAY_NONE || s_hold_active) ? 120 : 500));
    }
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Init ST7735 %dx%d", BOARD_LCD_H_RES, BOARD_LCD_V_RES);

    spi_bus_config_t buscfg = ST7735_PANEL_BUS_SPI_CONFIG(
        BOARD_LCD_PIN_SCLK, BOARD_LCD_PIN_MOSI,
        BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config =
        ST7735_PANEL_IO_SPI_CONFIG(BOARD_LCD_PIN_CS, BOARD_LCD_PIN_DC, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST,
                                             &io_config, &io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .color_space = ESP_LCD_COLOR_SPACE_BGR,
#endif
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    esp_lcd_panel_set_gap(s_panel, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y);
    esp_lcd_panel_swap_xy(s_panel, true);
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    gpio_config_t bl_conf = {
        .pin_bit_mask = 1ULL << BOARD_LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_conf));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_LCD_PIN_BL, BOARD_LCD_BL_ON_LEVEL));

    fb_clear(COLOR_BG);
    fb_draw_text(24, 36, "BOOTING...", COLOR_ACCENT, COLOR_BG);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fb);
    ESP_LOGI(TAG, "ST7735 ready");
    return ESP_OK;
}

void display_task_start(void)
{
    xTaskCreatePinnedToCore(display_task, "lcd", 4096, NULL, 2, NULL, 1);
}

#else /* !CONFIG_BRIDGE_LCD_ENABLED */

esp_err_t display_init(void) { return ESP_OK; }
void display_task_start(void) {}
void display_set_overlay(display_overlay_t kind, int seconds_left) { (void)kind; (void)seconds_left; }
void display_set_message(const char *l1, const char *l2) { (void)l1; (void)l2; }
void display_next_screen(void) {}
display_screen_t display_current_screen(void) { return SCREEN_OVERVIEW; }
void display_set_hold(bool active, int pct, bool reset_zone) { (void)active; (void)pct; (void)reset_zone; }
void display_set_hold_hint(const char *hint) { (void)hint; }

#endif
