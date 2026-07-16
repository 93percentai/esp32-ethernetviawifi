#include "status_led.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_BRIDGE_STATUS_LED

static const char *TAG = "led";
static wifi_mgr_state_t s_state = WIFI_MGR_IDLE;

/* Minimal APA102 bit-bang: start frame, 1 LED, end frame */
static void apa102_write(uint8_t brightness, uint8_t r, uint8_t g, uint8_t b)
{
    /* start */
    for (int i = 0; i < 32; i++) {
        gpio_set_level(BOARD_LED_PIN_DATA, 0);
        gpio_set_level(BOARD_LED_PIN_CLK, 1);
        gpio_set_level(BOARD_LED_PIN_CLK, 0);
    }
    uint8_t frame[4] = {
        (uint8_t)(0xE0 | (brightness & 0x1F)),
        b, g, r
    };
    for (int byte = 0; byte < 4; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            gpio_set_level(BOARD_LED_PIN_DATA, (frame[byte] >> bit) & 1);
            gpio_set_level(BOARD_LED_PIN_CLK, 1);
            gpio_set_level(BOARD_LED_PIN_CLK, 0);
        }
    }
    /* end */
    for (int i = 0; i < 32; i++) {
        gpio_set_level(BOARD_LED_PIN_DATA, 1);
        gpio_set_level(BOARD_LED_PIN_CLK, 1);
        gpio_set_level(BOARD_LED_PIN_CLK, 0);
    }
}

esp_err_t status_led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_LED_PIN_CLK) | (1ULL << BOARD_LED_PIN_DATA),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    apa102_write(0, 0, 0, 0);
    ESP_LOGI(TAG, "APA102 ready");
    return ESP_OK;
}

void status_led_set_state(wifi_mgr_state_t state)
{
    s_state = state;
}

static void led_task(void *arg)
{
    (void)arg;
    int tick = 0;
    while (true) {
        wifi_mgr_status_t st;
        wifi_mgr_get_status(&st);
        s_state = st.state;
        tick++;

        switch (s_state) {
        case WIFI_MGR_CONNECTED:
            apa102_write(8, 0, 48, 12); /* solid green */
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        case WIFI_MGR_CONNECTING:
        case WIFI_MGR_DISCONNECTED:
            /* slow amber blink */
            apa102_write((tick & 1) ? 8 : 0, 48, 28, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        case WIFI_MGR_SCANNING:
            apa102_write((tick & 1) ? 8 : 0, 0, 20, 48);
            vTaskDelay(pdMS_TO_TICKS(150));
            break;
        case WIFI_MGR_BAD_AUTH:
        case WIFI_MGR_NO_AP:
            apa102_write((tick & 1) ? 10 : 0, 48, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        case WIFI_MGR_IDLE:
        default:
            /* fast dim cyan = needs provisioning */
            apa102_write((tick & 1) ? 6 : 0, 0, 24, 32);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

void status_led_task_start(void)
{
    xTaskCreate(led_task, "led", 2048, NULL, 1, NULL);
}

#else

esp_err_t status_led_init(void) { return ESP_OK; }
void status_led_set_state(wifi_mgr_state_t state) { (void)state; }
void status_led_task_start(void) {}

#endif
