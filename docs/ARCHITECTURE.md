# Architecture

## Goal

Turn a LilyGO T-Dongle-S3 into a **driverless USB Wi-Fi adapter**: plug into any host with CDC-NCM support, associate to a 2.4 GHz AP on the dongle, and give the host a normal Ethernet-like interface on that AP’s LAN.

This follows the same model as [pico-usb-wifi](https://gitlab.com/baiyibai/pico-usb-wifi): the device is a transparent **layer-2 bridge**, not a NAT router and not a USB Wi-Fi chipset that exposes `cfg80211` to the host.

## Why emulate Ethernet (CDC-NCM) instead of USB Wi-Fi?

| Approach | Host sees | Host needs | Trade-off |
|----------|-----------|------------|-----------|
| Real USB Wi-Fi dongle | `wlan0` + full 802.11 stack | Chipset driver, firmware, `wpa_supplicant`, regulatory DB | Full Wi-Fi features; heavy host deps |
| **This project (NCM bridge)** | `usb0` / Ethernet NIC | In-box CDC-NCM (+ CDC-ACM) only | No monitor mode / host-side scan; Wi-Fi lives on the ESP32 |

The pico-usb-wifi README puts it clearly: constrained or appliance hosts often lack wireless drivers. Emulating an Ethernet gadget keeps the wireless complexity on the MCU.

## USB device classes used

Composite device (TinyUSB via Espressif `esp_tinyusb`):

1. **CDC-NCM** (Network Control Model) — Ethernet-over-USB  
   - Standardized subclass of CDC for networking  
   - Native support: Linux `cdc_ncm`, macOS, Windows 10+  
   - Preferred over RNDIS (Windows-centric) and ECM (macOS/Linux; Windows needs extra drivers)

2. **CDC-ACM** — management serial console  
   - Out-of-band provisioning (`set ssid`, `scan`, `save`, …)  
   - Required because the ESP32-S3 USB-OTG PHY is shared with USB-Serial/JTAG; once OTG TinyUSB owns the PHY, the built-in Serial/JTAG port is gone

When **no SSID** is configured, a third path runs on the Wi‑Fi radio itself: SoftAP **`ESP_WIFITOUSB_CONF`** at **`192.168.1.1`** with a DNS/HTTP captive portal. Nearby SSIDs are scanned first; submitted credentials are tested via STA association (APSTA) before SoftAP is torn down and normal bridging begins.

### Related USB network classes (research)

| Class | Linux | Windows | macOS | Notes |
|-------|-------|---------|-------|-------|
| CDC-NCM | Yes | Yes (10+) | Yes | Best cross-platform choice; used here |
| CDC-ECM | Yes | No (stock) | Yes | Espressif `usb_dongle` default on some builds |
| RNDIS | Yes | Yes | No | Windows-friendly; Linux DHCP quirks when roaming |

Espressif’s `esp-iot-solution` USB dongle documents these platform differences explicitly.

## The Wi-Fi station bridging constraint

A Wi-Fi **station** association grants **one MAC address**. Without 4-address/WDS frames (rarely available), the radio cannot transparently bridge frames for other MACs behind it.

**Solution (MAC adoption):** advertise the ESP32 STA MAC as the USB NCM interface MAC. Host and station share one identity. Frames can be forwarded **verbatim** at L2.

Consequences:

- One IP, held by the **host** (DHCP/SLAAC on the AP subnet)
- No NAT, no private tether subnet, no port forwards on the dongle
- IPv4 and IPv6 both work as Ethernet frames (subject to multicast filters — see below)
- The ESP32 itself holds **no** IP on the bridged path

## Data path

```
Host TX ──► TinyUSB NCM RX ──► usb_recv_callback()
                                   │
                                   ▼
                         esp_wifi_internal_tx(STA)
                                   │
                                   ▼
                              802.11 AP

AP / Wi-Fi RX ──► esp_wifi_internal_reg_rxcb handler
                                   │
                     ┌─────────────┴─────────────┐
                     │ source MAC == STA MAC?    │──yes──► drop (reflected)
                     └─────────────┬─────────────┘
                                   │ no
                                   ▼
                         tinyusb_net_send_sync()
                                   │
                                   ▼
                              Host RX
```

Key APIs (ESP-IDF private Wi-Fi):

- `esp_wifi_internal_tx(ESP_IF_WIFI_STA, buf, len)` — inject Ethernet frame onto the air
- `esp_wifi_internal_reg_rxcb(...)` — receive Ethernet frames from the driver **before** the TCP/IP stack
- `tinyusb_net_init` / `tinyusb_net_send_sync` — USB NCM class

This is the same pattern as Espressif’s official `tusb_ncm` example. pico-usb-wifi does the analogous thing with `cyw43_send_ethernet` and a replaced netif `input` handler.

## Self-reflection filter

Because host and STA share a MAC, broadcasts/multicasts the host sends are flooded by the AP back to the station. Without filtering, those frames would be delivered to the host again (echo).

Any Wi-Fi→USB frame whose **source MAC equals the STA MAC** is dropped and counted as `refl`.

## Multicast / IPv6 notes

pico-usb-wifi sets the CYW43 `allmulti` iovar so IPv6 ND/RA traffic is not filtered. ESP32’s station RX filter is less exposed; the Espressif `tusb_ncm` / `sta2eth` examples rely on the default STA filter, which is usually enough for DHCP and common IPv6 on flat home APs. Networks with aggressive IGMP/MLD snooping may need further work (promiscuous/multicast subscribe). Documented as a known limitation.

## Concurrency model (ESP32-S3)

Unlike the Pico’s single main-loop TinyUSB constraint, Espressif’s `esp_tinyusb` runs `tud_task` in its own FreeRTOS task. This firmware:

| Task / context | Role | Core |
|----------------|------|------|
| TinyUSB task | USB device stack | CPU0 (sdkconfig) |
| Wi-Fi driver / rxcb | Frame RX into bridge | Wi-Fi task |
| `console` | CDC-ACM line protocol | any |
| `httpd` / `dns_server` | SoftAP captive portal (only when unconfigured) | any |
| `lcd` | ST7735 redraw @ 2 Hz | CPU1 |
| `led` | APA102 patterns | any |

`tinyusb_net_send_sync` is used from the Wi-Fi RX callback with a 100 ms timeout (same as Espressif `sta2eth`) so a stalled USB host cannot exhaust Wi-Fi buffers indefinitely; failures increment `drop_rx`.

### NCM link state

After `tinyusb_net_init`, the firmware forces **NCM link down**, then calls `tud_network_link_state(0, true)` only when the STA associates (and `false` on disconnect). This matches Espressif’s updated `tusb_ncm` example and the Apple NCM DHCP fix ([IDFGH-17035](https://github.com/espressif/esp-idf/issues/18079)): hosts that DHCP only on the CDC `NETWORK_CONNECTION` notification otherwise race an unready bridge.

## Configuration storage

Profiles live in NVS namespace `bridge`, key `cfg_v1`:

- Up to 8 SSID/password pairs
- Active profile index
- Debug flag (reserved)

Compile-time defaults from `menuconfig` seed the first profile when NVS is empty.

## LCD / LED presentation layer

The display does **not** participate in the data path. Every 500 ms it samples:

- `wifi_mgr_get_status()` — link state, SSID, RSSI
- `bridge_get_stats()` — byte counters + EMA rates

Rates are exponential moving averages over ~0.5 s windows so the UI is readable under bursty traffic.

## Comparison to pico-usb-wifi

| Topic | pico-usb-wifi | This port |
|-------|---------------|-----------|
| MCU | RP2040 + CYW43439 | ESP32-S3 |
| USB | Native FS TinyUSB | ESP32-S3 USB-OTG FS TinyUSB |
| Network class | CDC-NCM | CDC-NCM |
| Bridge | L2 + MAC adoption | L2 + MAC adoption |
| Console | Dual CDC-ACM (mgmt + debug) | Single CDC-ACM (mgmt; stats on LCD) |
| Status UI | Onboard LED | ST7735 + APA102 |
| Throughput ceiling | ~4–5 Mbit/s (USB FS) | USB FS limited similarly (~same order) |

## Multi-mode gadget (SD MSC, network share, NAT tether, HID)

On top of the base NCM bridge, the firmware adds runtime-toggleable modes. Each has its own LCD screen; a short BOOT tap cycles screens and a 2 s hold on a mode screen toggles it.

### USB endpoint budget + LRU eviction

The ESP32-S3 USB-OTG controller exposes only **5 IN endpoints including EP0** (5 TX FIFOs), i.e. **4 IN endpoints for functions**. CDC-NCM is pinned (2 IN); the optional functions share the remaining **2 IN endpoints**:

- CDC-ACM console = 2, MSC = 1, HID = 1

Valid optional combos: `{}`, `{ACM}`, `{MSC}`, `{HID}`, `{MSC,HID}`. Each function carries a monotonic enable sequence (`enable_seq`) in NVS. When enabling a function would exceed the budget, [usb_gadget.c](../firmware/main/usb_gadget.c) evicts the **oldest-enabled** optional function(s) first (LRU) and shows the dropped set on the toggle overlay. `usb_gadget` builds a custom composite configuration descriptor for the resolved subset; `main/tusb_override/tusb_config.h` forces `CFG_TUD_MSC=1` so the firmware can supply its own `tud_msc_*` callbacks instead of esp_tinyusb's built-in storage helper.

Because TinyUSB descriptors and the network mode are effectively static, **toggling a mode persists to NVS and reboots** to re-enumerate.

### Network modes: L2 bridge vs NAT tether

- **L2 bridge** (default, and when only MSC is on): unchanged — no ESP IP, MAC adoption, raw `esp_wifi_internal_*`.
- **NAT tether** (auto-selected when the network share or HID mode is on): the STA gets a real DHCP IP (`esp_netif_create_default_wifi_sta`), and a USB-side `esp_netif` (`192.168.7.1/24` + DHCP server) NAPTs the host onto the LAN ([net_tether.c](../firmware/main/net_tether.c), `CONFIG_LWIP_IPV4_NAPT`). The NCM receive callback branches: L2 → `esp_wifi_internal_tx`; NAT → `esp_netif_receive`.

### SD card single-owner arbiter

MSC hands the raw block device to the USB host; the on-device FAT mount (WebDAV/web share) cannot touch the card at the same time. [sdcard.c](../firmware/main/sdcard.c) enforces a single owner (`SD_OWNER_HOST` vs `SD_OWNER_ESP`) and maintains per-transfer read/write I/O counters. The custom MSC callbacks ([msc.c](../firmware/main/msc.c)) go through `sdcard_read_sectors`/`sdcard_write_sectors`, so host activity is visible. The web UI ([httpd_share.c](../firmware/main/httpd_share.c)) shows an **"SD in use by USB storage"** banner, a **Force unmount** button (switches ownership to the ESP), and **live read/write activity** with a confirm prompt when a transfer is in flight.

### Web share + WebDAV + HID

[httpd_share.c](../firmware/main/httpd_share.c) serves a copyparty-style file manager (`/`, `/api/*`, `/dl`) and a WebDAV subset (`/dav*`: OPTIONS/PROPFIND/GET/HEAD/PUT/DELETE/MKCOL/MOVE) from the FAT SD, reachable on the STA LAN IP and the USB NAT IP. It also exposes HID endpoints (`/api/hid/text|key|mouse`) that queue reports to [hid.c](../firmware/main/hid.c), which drives the USB HID keyboard+mouse to the plugged-in host.

### Button gestures

- **Short tap** — next info screen.
- **Hold 2–5 s, release on a mode screen** — toggle that mode (save + reboot).
- **Hold ≥5 s, release, then tap within 5 s** — factory reset (clears Wi-Fi profiles and modes).

### Hardware verification note

Build + QEMU boot are validated in CI/cloud, but QEMU cannot exercise USB-OTG, the SD slot, or the Wi-Fi radio. Composite enumeration (each combo), NAT DHCP/NAPT, WebDAV mounting, MSC, and HID injection must be verified on a physical T-Dongle-S3.

## Security notes

- Credentials are stored in NVS (not encrypted by default). Treat a lost dongle as a credential leak.
- The management console has no authentication — physical USB access is the trust boundary.
- This is intentionally **not** a USB Wi-Fi device that can run monitor mode or inject arbitrary 802.11 management frames for the host.
