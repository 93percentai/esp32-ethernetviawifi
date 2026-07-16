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
| `lcd` | ST7735 redraw @ 2 Hz | CPU1 |
| `led` | APA102 patterns | any |

`tinyusb_net_send_sync` is used from the Wi-Fi RX callback with a short timeout so a stalled USB host cannot exhaust Wi-Fi buffers indefinitely; failures increment `drop_rx`.

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

## Security notes

- Credentials are stored in NVS (not encrypted by default). Treat a lost dongle as a credential leak.
- The management console has no authentication — physical USB access is the trust boundary.
- This is intentionally **not** a USB Wi-Fi device that can run monitor mode or inject arbitrary 802.11 management frames for the host.
