#include "hid.h"

#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_gadget.h"

static const char *TAG = "hid";

typedef enum {
    HID_EV_KEY_TAP = 0,
    HID_EV_MOUSE,
    HID_EV_LEFT_CLICK,
} hid_ev_type_t;

typedef struct {
    hid_ev_type_t type;
    union {
        struct {
            uint8_t modifier;
            uint8_t keycode;  /* 0 = release */
        } key;
        struct {
            uint8_t buttons;
            int8_t dx;
            int8_t dy;
            int8_t wheel;
        } mouse;
    };
} hid_event_t;

static QueueHandle_t s_queue;
static volatile int64_t s_last_activity_us = -1;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static hid_stats_t s_stats;

/* ASCII -> HID usage + shift flag (TinyUSB provided table). */
static const uint8_t s_ascii2kc[128][2] = { HID_ASCII_TO_KEYCODE };

bool hid_host_ready(void)
{
    return usb_gadget_func_active(USB_FUNC_HID) && tud_mounted() && tud_hid_ready();
}

void hid_get_stats(hid_stats_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_stats_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
    out->queue_depth = s_queue ? (uint32_t)uxQueueMessagesWaiting(s_queue) : 0;
}

uint32_t hid_ms_since_activity(void)
{
    if (s_last_activity_us < 0) {
        return UINT32_MAX;
    }
    int64_t dt = (esp_timer_get_time() - s_last_activity_us) / 1000;
    if (dt < 0) {
        dt = 0;
    }
    return (dt > UINT32_MAX) ? UINT32_MAX : (uint32_t)dt;
}

static bool wait_ready(int timeout_ms)
{
    int waited = 0;
    while (!tud_hid_ready()) {
        if (!tud_mounted() || waited >= timeout_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        waited += 2;
    }
    return true;
}

static void send_key(uint8_t modifier, uint8_t keycode)
{
    if (!wait_ready(200)) {
        return;
    }
    uint8_t keys[6] = { keycode, 0, 0, 0, 0, 0 };
    tud_hid_keyboard_report(HID_REPORT_ID_KEYBOARD, modifier, keycode ? keys : NULL);
    s_last_activity_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.sent_reports++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!wait_ready(200)) {
        return;
    }
    tud_hid_mouse_report(HID_REPORT_ID_MOUSE, buttons, dx, dy, wheel, 0);
    s_last_activity_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.sent_reports++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void hid_task(void *arg)
{
    (void)arg;
    hid_event_t ev;
    while (true) {
        if (xQueueReceive(s_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!usb_gadget_func_active(USB_FUNC_HID)) {
            continue;
        }
        if (ev.type == HID_EV_KEY_TAP) {
            send_key(ev.key.modifier, ev.key.keycode);
            vTaskDelay(pdMS_TO_TICKS(8));
            send_key(0, 0);
        } else if (ev.type == HID_EV_MOUSE) {
            send_mouse(ev.mouse.buttons, ev.mouse.dx, ev.mouse.dy, ev.mouse.wheel);
        } else {
            send_mouse(MOUSE_BUTTON_LEFT, 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(8));
            send_mouse(0, 0, 0, 0);
        }
        /* Small spacing so hosts register discrete events. */
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

esp_err_t hid_init(void)
{
    if (s_queue) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(64, sizeof(hid_event_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(hid_task, "hid", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "HID gadget ready (keyboard+mouse)");
    return ESP_OK;
}

static esp_err_t queue_ev(const hid_event_t *ev)
{
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_queue, ev, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_stats_lock);
        s_stats.dropped_events++;
        portEXIT_CRITICAL(&s_stats_lock);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.queued_events++;
    portEXIT_CRITICAL(&s_stats_lock);
    return ESP_OK;
}

esp_err_t hid_queue_key(uint8_t modifier, uint8_t keycode)
{
    hid_event_t tap = { .type = HID_EV_KEY_TAP, .key = { modifier, keycode } };
    return queue_ev(&tap);
}

esp_err_t hid_queue_text(const char *utf8, bool press_enter_after)
{
    if (!utf8) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_OK;
    for (const char *p = utf8; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c >= 128) {
            continue;  /* non-ASCII not mapped */
        }
        /* TinyUSB's HID_ASCII_TO_KEYCODE entries are
         * { shift_required, HID keycode }, in that order. */
        uint8_t keycode = s_ascii2kc[c][1];
        uint8_t modifier = s_ascii2kc[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
        if (keycode == 0) {
            continue;
        }
        if (hid_queue_key(modifier, keycode) != ESP_OK) {
            result = ESP_ERR_NO_MEM;
            break;
        }
    }
    if (result == ESP_OK && press_enter_after) {
        result = hid_queue_key(0, HID_KEY_ENTER);
    }
    return result;
}

esp_err_t hid_queue_mouse(uint8_t buttons, int dx, int dy, int wheel)
{
    hid_event_t ev = {
        .type = HID_EV_MOUSE,
        .mouse = {
            .buttons = buttons,
            .dx = (int8_t)(dx < -127 ? -127 : (dx > 127 ? 127 : dx)),
            .dy = (int8_t)(dy < -127 ? -127 : (dy > 127 ? 127 : dy)),
            .wheel = (int8_t)(wheel < -127 ? -127 : (wheel > 127 ? 127 : wheel)),
        },
    };
    return queue_ev(&ev);
}

esp_err_t hid_queue_left_click(void)
{
    hid_event_t ev = { .type = HID_EV_LEFT_CLICK };
    return queue_ev(&ev);
}

/* ---- TinyUSB HID class callbacks ---- */

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;  /* no feature/input reports served over the control pipe */
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
    /* Keyboard LED (caps/num lock) output reports are ignored. */
}
