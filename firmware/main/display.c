#include "display.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "bridge.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"
#include "esp_log.h"
#include "font8x8.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_mgr.h"

#if CONFIG_BRIDGE_LCD_ENABLED

static const char *TAG = "display";

#define COLOR_BG     0x1082  /* deep slate */
#define COLOR_PANEL  0x2104
#define COLOR_TEXT   0xEF7D  /* warm off-white */
#define COLOR_MUTED  0x8410
#define COLOR_OK     0x05E0  /* green */
#define COLOR_WARN   0xFE60  /* amber */
#define COLOR_ERR    0xF800  /* red */
#define COLOR_ACCENT 0x07FF  /* cyan */

static esp_lcd_panel_handle_t s_panel;
static uint16_t s_fb[BOARD_LCD_H_RES * BOARD_LCD_V_RES];

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
        if (row < 0) {
            continue;
        }
        for (int col = x; col < x + w && col < BOARD_LCD_H_RES; col++) {
            if (col < 0) {
                continue;
            }
            s_fb[row * BOARD_LCD_H_RES + col] = c;
        }
    }
}

static void fb_draw_char(int x, int y, char ch, uint16_t fg, uint16_t bg)
{
    if (ch < 32 || ch > 127) {
        ch = '?';
    }
    const uint8_t *glyph = font8x8_basic[ch - 32];
    uint16_t f = swap565(fg);
    uint16_t b = swap565(bg);
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        int py = y + row;
        if (py < 0 || py >= BOARD_LCD_V_RES) {
            continue;
        }
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px < 0 || px >= BOARD_LCD_H_RES) {
                continue;
            }
            s_fb[py * BOARD_LCD_H_RES + px] = (bits & (1u << col)) ? f : b;
        }
    }
}

static void fb_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg)
{
    while (*text) {
        fb_draw_char(x, y, *text++, fg, bg);
        x += 8;
        if (x > BOARD_LCD_H_RES - 8) {
            break;
        }
    }
}

static const char *link_label(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_IDLE: return "NO CONFIG";
    case WIFI_MGR_CONNECTING: return "ASSOCIATING";
    case WIFI_MGR_CONNECTED: return "CONNECTED";
    case WIFI_MGR_DISCONNECTED: return "RECONNECT";
    case WIFI_MGR_NO_AP: return "NO AP";
    case WIFI_MGR_BAD_AUTH: return "BAD AUTH";
    case WIFI_MGR_SCANNING: return "SCANNING";
    default: return "?";
    }
}

static uint16_t link_color(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_CONNECTED: return COLOR_OK;
    case WIFI_MGR_CONNECTING:
    case WIFI_MGR_SCANNING:
    case WIFI_MGR_DISCONNECTED: return COLOR_WARN;
    case WIFI_MGR_BAD_AUTH:
    case WIFI_MGR_NO_AP: return COLOR_ERR;
    default: return COLOR_MUTED;
    }
}

static void render(void)
{
    wifi_mgr_status_t st;
    bridge_stats_t stats;
    wifi_mgr_get_status(&st);
    bridge_get_stats(&stats);

    fb_clear(COLOR_BG);
    fb_fill_rect(0, 0, BOARD_LCD_H_RES, 12, COLOR_PANEL);
    fb_draw_text(2, 2, "T-DONGLE WIFI", COLOR_ACCENT, COLOR_PANEL);

    char line[32];
    uint16_t lc = link_color(st.state);
    fb_draw_text(2, 16, "LINK", COLOR_MUTED, COLOR_BG);
    fb_draw_text(42, 16, link_label(st.state), lc, COLOR_BG);

    const char *ssid = st.ssid[0] ? st.ssid : "(unset)";
    snprintf(line, sizeof(line), "%.14s", ssid);
    fb_draw_text(2, 28, "SSID", COLOR_MUTED, COLOR_BG);
    fb_draw_text(42, 28, line, COLOR_TEXT, COLOR_BG);

    if (st.state == WIFI_MGR_CONNECTED) {
        snprintf(line, sizeof(line), "%d dBm", st.rssi);
    } else {
        snprintf(line, sizeof(line), "--");
    }
    fb_draw_text(2, 40, "RSSI", COLOR_MUTED, COLOR_BG);
    fb_draw_text(42, 40, line, COLOR_TEXT, COLOR_BG);

    char bytes[12], rate[12];
    bridge_format_bytes(stats.bytes_to_host, bytes, sizeof(bytes));
    bridge_format_rate(stats.rate_to_host_bps, rate, sizeof(rate));
    snprintf(line, sizeof(line), "%.10s %.10s", bytes, rate);
    fb_draw_text(2, 52, "DN", COLOR_OK, COLOR_BG);
    fb_draw_text(26, 52, line, COLOR_TEXT, COLOR_BG);

    bridge_format_bytes(stats.bytes_to_wifi, bytes, sizeof(bytes));
    bridge_format_rate(stats.rate_to_wifi_bps, rate, sizeof(rate));
    snprintf(line, sizeof(line), "%.10s %.10s", bytes, rate);
    fb_draw_text(2, 64, "UP", COLOR_WARN, COLOR_BG);
    fb_draw_text(26, 64, line, COLOR_TEXT, COLOR_BG);

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fb);
}

static void display_task(void *arg)
{
    (void)arg;
    while (true) {
        render();
        vTaskDelay(pdMS_TO_TICKS(500));
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
        .color_space = ESP_LCD_COLOR_SPACE_BGR,
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

    /* Backlight via LEDC, active-low */
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch = {
        .gpio_num = BOARD_LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = BOARD_LCD_BL_ON_LEVEL ? 200 : (255 - 200),
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                                  BOARD_LCD_BL_ON_LEVEL ? 220 : (255 - 220)));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));

    fb_clear(COLOR_BG);
    fb_draw_text(24, 36, "BOOTING...", COLOR_ACCENT, COLOR_BG);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fb);
    return ESP_OK;
}

void display_task_start(void)
{
    xTaskCreatePinnedToCore(display_task, "lcd", 4096, NULL, 2, NULL, 1);
}

#else /* !CONFIG_BRIDGE_LCD_ENABLED */

esp_err_t display_init(void) { return ESP_OK; }
void display_task_start(void) {}

#endif
