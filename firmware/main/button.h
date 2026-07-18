#pragma once

#include "config_store.h"
#include "esp_err.h"

esp_err_t button_init(bridge_config_t *cfg);
void button_task_start(void);

/* Remote BOOT-button injection from the web UI (no physical press). */
void button_remote_tap(void);
/* Simulate a 2 s hold on a mode screen (toggles mode and reboots). */
esp_err_t button_remote_hold_toggle(void);
