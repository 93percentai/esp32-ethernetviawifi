#include "button.h"

#include "board.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "provisioning.h"

static const char *TAG = "button";

#define BTN_ACTIVE_LEVEL     0
#define BTN_DEBOUNCE_MS      40
#define BTN_CONFIRM_WINDOW_MS 5000
#define BTN_DONE_HOLD_MS     1500
#define BTN_POLL_MS          20

static bridge_config_t *s_cfg;
static bool s_armed;
static int64_t s_deadline_ms;

static int64_t now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool btn_raw_pressed(void)
{
    return gpio_get_level(BOARD_BTN_PIN) == BTN_ACTIVE_LEVEL;
}

static void clear_wifi_and_reprovision(void)
{
    ESP_LOGW(TAG, "Confirmed — clearing Wi-Fi settings");
    display_set_overlay(DISPLAY_OVERLAY_RESET_DONE, 0);

    config_clear_wifi(s_cfg);
    config_save(s_cfg);
    provisioning_apply_or_start(s_cfg);

    vTaskDelay(pdMS_TO_TICKS(BTN_DONE_HOLD_MS));
    display_set_overlay(DISPLAY_OVERLAY_NONE, 0);
}

static void button_task(void *arg)
{
    (void)arg;
    bool stable = false;
    bool last_stable = false;
    bool sample = false;
    int debounce_left = 0;

    /* Ignore a held BOOT from download-mode / power-on strapping. */
    while (btn_raw_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    last_stable = false;
    stable = false;
    sample = false;

    while (true) {
        bool raw = btn_raw_pressed();
        if (raw != sample) {
            sample = raw;
            debounce_left = BTN_DEBOUNCE_MS / BTN_POLL_MS;
        } else if (debounce_left > 0) {
            debounce_left--;
            if (debounce_left == 0) {
                stable = sample;
            }
        }

        int64_t t = now_ms();

        if (s_armed && t >= s_deadline_ms) {
            ESP_LOGI(TAG, "Reset confirm timed out");
            s_armed = false;
            display_set_overlay(DISPLAY_OVERLAY_NONE, 0);
        } else if (s_armed) {
            int left = (int)((s_deadline_ms - t + 999) / 1000);
            if (left < 0) {
                left = 0;
            }
            display_set_overlay(DISPLAY_OVERLAY_RESET_CONFIRM, left);
        }

        /* Rising press edge (debounced): active-low, so false→true means pressed. */
        if (stable && !last_stable) {
            if (!s_armed) {
                s_armed = true;
                s_deadline_ms = t + BTN_CONFIRM_WINDOW_MS;
                display_set_overlay(DISPLAY_OVERLAY_RESET_CONFIRM, 5);
                ESP_LOGI(TAG, "Reset arm: press again within %d s",
                         BTN_CONFIRM_WINDOW_MS / 1000);
            } else {
                s_armed = false;
                clear_wifi_and_reprovision();
            }
        }

        last_stable = stable;
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

esp_err_t button_init(bridge_config_t *cfg)
{
    s_cfg = cfg;
    s_armed = false;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BTN_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_LOGI(TAG, "BOOT button on GPIO%d ready (press twice to reset Wi-Fi)",
             BOARD_BTN_PIN);
    return ESP_OK;
}

void button_task_start(void)
{
    xTaskCreate(button_task, "button", 3072, NULL, 2, NULL);
}
