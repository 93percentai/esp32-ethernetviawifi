#pragma once

#include "esp_err.h"

typedef enum {
    DISPLAY_OVERLAY_NONE = 0,
    DISPLAY_OVERLAY_RESET_CONFIRM,
    DISPLAY_OVERLAY_RESET_DONE,
} display_overlay_t;

esp_err_t display_init(void);
void display_task_start(void);

/* Transient full-screen overlays (button reset confirm, etc.). */
void display_set_overlay(display_overlay_t kind, int seconds_left);
