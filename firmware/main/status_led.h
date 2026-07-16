#pragma once

#include "esp_err.h"
#include "wifi_mgr.h"

esp_err_t status_led_init(void);
void status_led_set_state(wifi_mgr_state_t state);
void status_led_task_start(void);
