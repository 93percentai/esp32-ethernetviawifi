#include "button.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "provisioning.h"
#include "usb_gadget.h"

static const char *TAG = "button";

#define BTN_ACTIVE_LEVEL      0
#define BTN_POLL_MS           20
#define BTN_DEBOUNCE_MS       40
#define HOLD_TOGGLE_MS        2000   /* release in [2s,5s) on a mode screen: toggle */
#define HOLD_RESET_MS         5000   /* hold >= 5s then release: arm factory reset */
#define RESET_CONFIRM_MS      5000   /* second press window to confirm reset */
#define OVERLAY_HOLD_MS       1500

static bridge_config_t *s_cfg;
static bool s_reset_armed;
static bool s_action_fired;   /* an action already fired during the current hold */
static int64_t s_reset_deadline_ms;

static bool is_mode_screen(display_screen_t s)
{
    return s == SCREEN_SD || s == SCREEN_SHARE || s == SCREEN_HID;
}

/* Live hint shown while holding, so the user knows exactly when to release. */
static void update_hold_hint(display_screen_t scr, int64_t held)
{
    char hint[24];
    if (is_mode_screen(scr)) {
        bool on;
        const char *name;
        if (scr == SCREEN_SD)        { on = config_usb_func_enabled(s_cfg, USB_FUNC_MSC); name = "USB SD"; }
        else if (scr == SCREEN_HID)  { on = config_usb_func_enabled(s_cfg, USB_FUNC_HID); name = "HID"; }
        else                         { on = config_share_enabled(s_cfg);                  name = "SHARE"; }
        if (held >= HOLD_TOGGLE_MS) {
            snprintf(hint, sizeof(hint), "release: %s %s", name, on ? "OFF" : "ON");
        } else {
            snprintf(hint, sizeof(hint), "hold 2s: %s %s", name, on ? "OFF" : "ON");
        }
    } else {
        if (held >= HOLD_RESET_MS) {
            snprintf(hint, sizeof(hint), "release: FACTORY RESET");
        } else {
            snprintf(hint, sizeof(hint), "hold 5s: reset");
        }
    }
    display_set_hold_hint(hint);
}

static int64_t now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool btn_raw_pressed(void)
{
    return gpio_get_level(BOARD_BTN_PIN) == BTN_ACTIVE_LEVEL;
}

static void reboot_after_overlay(void)
{
    vTaskDelay(pdMS_TO_TICKS(OVERLAY_HOLD_MS));
    esp_restart();
}

static void do_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset: clearing Wi-Fi + modes");
    display_set_overlay(DISPLAY_OVERLAY_RESET_DONE, 0);
    config_clear_wifi(s_cfg);   /* also resets runtime modes to defaults */
    config_save(s_cfg);
    reboot_after_overlay();
}

static void append_name(char *dst, size_t n, const char *name)
{
    if (dst[0]) {
        strncat(dst, ",", n - strlen(dst) - 1);
    }
    strncat(dst, name, n - strlen(dst) - 1);
}

/* Toggle the mode owned by the current screen, persist, and reboot to apply
 * (USB descriptor / network mode changes require re-enumeration). */
static void toggle_current_mode(void)
{
    display_screen_t scr = display_current_screen();
    char l1[24] = "";
    char l2[24] = "";

    if (scr == SCREEN_SD || scr == SCREEN_HID) {
        usb_func_t func = (scr == SCREEN_SD) ? USB_FUNC_MSC : USB_FUNC_HID;
        bool enabled = config_usb_func_enabled(s_cfg, func);
        uint8_t evicted = usb_gadget_resolve_enable(s_cfg, func, !enabled);
        snprintf(l1, sizeof(l1), "%s %s", usb_func_name(func), enabled ? "OFF" : "ON");
        if (evicted) {
            char names[20] = "";
            for (int f = 0; f < USB_FUNC_COUNT; f++) {
                if (evicted & (1u << f)) {
                    append_name(names, sizeof(names), usb_func_name((usb_func_t)f));
                }
            }
            snprintf(l2, sizeof(l2), "dropped %s", names);
        }
    } else if (scr == SCREEN_SHARE) {
        bool enabled = config_share_enabled(s_cfg);
        config_share_set(s_cfg, !enabled);
        snprintf(l1, sizeof(l1), "SHARE %s", enabled ? "OFF" : "ON");
        if (!enabled) {
            snprintf(l2, sizeof(l2), "NAT tether on");
        }
    } else {
        /* Not a toggleable mode screen. */
        return;
    }

    config_save(s_cfg);
    ESP_LOGI(TAG, "Mode toggle: %s / %s", l1, l2);
    display_set_message(l1, l2);
    display_set_overlay(DISPLAY_OVERLAY_MODE_APPLIED, 0);
    reboot_after_overlay();
}

static void button_task(void *arg)
{
    (void)arg;
    bool stable = false, last_stable = false, sample = false;
    int debounce_left = 0;
    int64_t press_start_ms = 0;

    /* Ignore a held BOOT from power-on / download-mode strapping. */
    while (btn_raw_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    while (true) {
        bool raw = btn_raw_pressed();
        if (raw != sample) {
            sample = raw;
            debounce_left = BTN_DEBOUNCE_MS / BTN_POLL_MS;
        } else if (debounce_left > 0) {
            if (--debounce_left == 0) {
                stable = sample;
            }
        }

        int64_t t = now_ms();

        /* Reset-confirm window bookkeeping. */
        if (s_reset_armed && t >= s_reset_deadline_ms) {
            s_reset_armed = false;
            display_set_overlay(DISPLAY_OVERLAY_NONE, 0);
        } else if (s_reset_armed) {
            int left = (int)((s_reset_deadline_ms - t + 999) / 1000);
            display_set_overlay(DISPLAY_OVERLAY_RESET_CONFIRM, left < 0 ? 0 : left);
        }

        /* Press edge. */
        if (stable && !last_stable) {
            press_start_ms = t;
            s_action_fired = false;
        }

        /* While held: drive the progress bar and fire the hold action as soon
         * as its threshold is reached (immediate feedback, no need to guess
         * when 2 s has elapsed). Mode screens toggle at 2 s; other screens arm
         * the factory reset at 5 s — the two gestures never overlap. */
        if (stable && !s_action_fired) {
            int64_t held = t - press_start_ms;
            display_screen_t scr = display_current_screen();
            int pct = (int)(held * 100 / (is_mode_screen(scr) ? HOLD_TOGGLE_MS : HOLD_RESET_MS));
            if (pct > 100) pct = 100;
            display_set_hold(true, pct, !is_mode_screen(scr) && held >= HOLD_RESET_MS);
            update_hold_hint(scr, held);

            if (is_mode_screen(scr) && held >= HOLD_TOGGLE_MS && !s_reset_armed) {
                s_action_fired = true;
                display_set_hold(false, 0, false);
                toggle_current_mode();  /* saves + reboots; does not return */
            } else if (!is_mode_screen(scr) && held >= HOLD_RESET_MS && !s_reset_armed) {
                s_action_fired = true;
                display_set_hold(false, 0, false);
                s_reset_armed = true;
                s_reset_deadline_ms = t + RESET_CONFIRM_MS;
                display_set_overlay(DISPLAY_OVERLAY_RESET_CONFIRM, RESET_CONFIRM_MS / 1000);
                ESP_LOGI(TAG, "Reset armed: tap again within %d s", RESET_CONFIRM_MS / 1000);
            }
        }

        /* Release edge. */
        if (!stable && last_stable) {
            display_set_hold(false, 0, false);
            if (s_action_fired) {
                /* Action already handled during the hold (toggle/reset-arm). */
            } else if (s_reset_armed) {
                /* A tap within the window confirms the pending factory reset. */
                s_reset_armed = false;
                do_factory_reset();  /* unreachable */
            } else {
                /* Short tap: advance to the next info screen. */
                display_next_screen();
            }
        }

        last_stable = stable;
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

esp_err_t button_init(bridge_config_t *cfg)
{
    s_cfg = cfg;
    s_reset_armed = false;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BTN_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_LOGI(TAG, "BOOT button ready (tap=screen, hold 2s=toggle, hold 5s+tap=reset)");
    return ESP_OK;
}

void button_task_start(void)
{
    xTaskCreate(button_task, "button", 4096, NULL, 2, NULL);
}
