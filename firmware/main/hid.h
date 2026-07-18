#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Report IDs used in the combined keyboard+mouse report descriptor. */
#define HID_REPORT_ID_KEYBOARD  1
#define HID_REPORT_ID_MOUSE     2

/*
 * Network-controlled USB HID gadget. Events queued from the web server are
 * drained by an internal task that emits USB HID reports to the plugged host,
 * so the ESP32 acts as a remote keyboard + mouse.
 */
esp_err_t hid_init(void);

/* True when the HID function is active in the USB descriptor and the host is
 * ready to receive reports. */
bool hid_host_ready(void);

/* Milliseconds since the last emitted HID report (UINT32_MAX if never). */
uint32_t hid_ms_since_activity(void);

/* Queue a single key tap: press (modifier + one usage) then release. */
void hid_queue_key(uint8_t modifier, uint8_t keycode);

/* Queue ASCII text; each character is translated to a key tap (with shift). */
void hid_queue_text(const char *utf8);

/* Queue a relative mouse report. */
void hid_queue_mouse(uint8_t buttons, int dx, int dy, int wheel);
