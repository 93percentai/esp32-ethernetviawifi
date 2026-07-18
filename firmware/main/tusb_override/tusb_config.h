/*
 * Project TinyUSB configuration override.
 *
 * This file is injected ahead of esp_tinyusb's own tusb_config.h on the include
 * path (see main/CMakeLists.txt). It pulls in esp_tinyusb's configuration via
 * #include_next and then forces the MSC class driver on.
 *
 * Rationale: esp_tinyusb only compiles the MSC class (CFG_TUD_MSC) together with
 * its built-in storage helper (tusb_msc_storage.c) when CONFIG_TINYUSB_MSC_ENABLED
 * is set, and that helper defines the tud_msc_* callbacks itself. This firmware
 * needs its own tud_msc_* callbacks so it can expose per-transfer read/write
 * activity (used by the web UI "SD in use / force unmount" safety flow). We keep
 * CONFIG_TINYUSB_MSC_ENABLED off (so the helper is not compiled) but still turn on
 * the class driver here.
 */
#pragma once

#include_next "tusb_config.h"

#undef CFG_TUD_MSC
#define CFG_TUD_MSC 1

#undef CFG_TUD_MSC_BUFSIZE
#define CFG_TUD_MSC_BUFSIZE 4096
