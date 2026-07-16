#pragma once

#include "config_store.h"
#include "esp_err.h"

esp_err_t button_init(bridge_config_t *cfg);
void button_task_start(void);
