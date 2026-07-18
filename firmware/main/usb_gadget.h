#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "config_store.h"

/*
 * usb_gadget owns the single TinyUSB driver install and builds a custom
 * composite configuration descriptor from the runtime mode flags. CDC-NCM is a
 * pinned base function (always present). The optional functions ACM/MSC/HID
 * compete for the ESP32-S3's limited USB IN endpoints:
 *
 *   Total IN endpoints (incl. EP0)            = 5   (5 TX FIFOs)
 *   Pinned NCM cost (notif + data IN)         = 2
 *   Remaining budget for optional functions   = 2
 *   Costs: ACM = 2, MSC = 1, HID = 1
 *
 * Valid optional combos therefore: {}, {ACM}, {MSC}, {HID}, {MSC,HID}.
 * usb_gadget_resolve_enable() applies an LRU policy that evicts the oldest
 * enabled optional function when a newly enabled one would exceed the budget.
 */
#define USB_OPTIONAL_IN_BUDGET  2

/* IN-endpoint cost of an optional USB function. */
uint8_t usb_func_in_cost(usb_func_t func);

/* Human-readable short name for a function ("ACM"/"MSC"/"HID"). */
const char *usb_func_name(usb_func_t func);

/*
 * Apply an LRU-aware enable/disable of a USB function to cfg (in place).
 * On enable, evicts the oldest other optional function(s) until the endpoint
 * budget is satisfied. Returns a bitmask (1u<<usb_func_t) of functions that
 * were evicted (0 when nothing had to be dropped or when disabling).
 * Does NOT persist cfg or reboot.
 */
uint8_t usb_gadget_resolve_enable(bridge_config_t *cfg, usb_func_t func, bool enable);

/*
 * Sanitize an already-stored mode set so it fits the endpoint budget
 * (evicting oldest-first if a corrupt/migrated config over-committed).
 * Returns bitmask of evicted functions.
 */
uint8_t usb_gadget_sanitize(bridge_config_t *cfg);

/* Install TinyUSB with the composite descriptor for the resolved mode set.
 * Must be called before any class init (bridge/console/msc/hid). */
esp_err_t usb_gadget_init(const bridge_config_t *cfg);

/* Which optional functions ended up active in the installed descriptor. */
bool usb_gadget_func_active(usb_func_t func);

/* HID report descriptor (keyboard + mouse); used by the HID class callback. */
const uint8_t *usb_gadget_hid_report_desc(uint16_t *len);
