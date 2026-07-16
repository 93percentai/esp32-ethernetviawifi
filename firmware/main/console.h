#pragma once

#include "config_store.h"
#include "esp_err.h"

esp_err_t console_init(bridge_config_t *cfg);
void console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
