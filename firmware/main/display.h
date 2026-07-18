#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Info screens cycled by a short press of the BOOT button. Each mode has its
 * own screen; a 2 s hold on a mode screen toggles that mode. */
typedef enum {
    SCREEN_OVERVIEW = 0,  /* Wi-Fi bridge link + traffic (default) */
    SCREEN_NETWORK,       /* L2 bridge vs NAT tether, IP addresses */
    SCREEN_SD,            /* SD card + USB mass-storage mode */
    SCREEN_SHARE,         /* WebDAV / web file share */
    SCREEN_HID,           /* USB HID remote keyboard/mouse */
    SCREEN_COUNT,
} display_screen_t;

typedef enum {
    DISPLAY_OVERLAY_NONE = 0,
    DISPLAY_OVERLAY_RESET_CONFIRM,
    DISPLAY_OVERLAY_RESET_DONE,
    DISPLAY_OVERLAY_MODE_APPLIED,  /* mode toggled; message shown before reboot */
} display_overlay_t;

esp_err_t display_init(void);
void display_task_start(void);

/* Transient full-screen overlays (button reset confirm, mode applied, etc.). */
void display_set_overlay(display_overlay_t kind, int seconds_left);

/* Two-line message used with DISPLAY_OVERLAY_MODE_APPLIED. */
void display_set_message(const char *line1, const char *line2);

/* Screen carousel (driven by the button short-press). */
void display_next_screen(void);
display_screen_t display_current_screen(void);

/* Button hold feedback: active shows a progress bar; reset_zone tints it red
 * once the hold has crossed the factory-reset threshold. pct is 0..100. */
void display_set_hold(bool active, int pct, bool reset_zone);

/* Short hint shown above the hold bar (e.g. "release: MSC ON"). "" clears it. */
void display_set_hold_hint(const char *hint);

/* Text mirror of what the LCD is showing (for the web UI). */
#define DISPLAY_SNAP_LINES 8
#define DISPLAY_SNAP_WIDTH 28
typedef struct {
    int screen;                         /* display_screen_t */
    char title[20];
    char lines[DISPLAY_SNAP_LINES][DISPLAY_SNAP_WIDTH];
    int line_count;
    char overlay[32];                   /* empty if none */
    char hold_hint[24];                 /* empty if none */
    bool hold_active;
    int hold_pct;
} display_snapshot_t;

void display_get_snapshot(display_snapshot_t *out);
