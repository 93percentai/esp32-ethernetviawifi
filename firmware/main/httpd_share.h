#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * SD card network share: a copyparty-style web file manager plus a WebDAV
 * endpoint, served by esp_http_server from the FAT-mounted SD card. Also hosts
 * the network-controlled HID endpoints. Reachable on the STA LAN IP and the USB
 * NAT IP (192.168.7.1).
 *
 * File operations require the on-device FAT mount (sdcard owner == ESP). When
 * the USB host owns the card (MSC), the UI shows a "SD in use by USB storage"
 * banner with a force-unmount control and live read/write activity.
 */
esp_err_t httpd_share_start(bool storage_share_enabled);
void httpd_share_stop(void);
bool httpd_share_running(void);
bool httpd_share_storage_enabled(void);
