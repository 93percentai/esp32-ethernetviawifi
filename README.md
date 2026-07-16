# esp32-ethernetviawifi

Driverless USB Wi-Fi adapter firmware for the **LilyGO T-Dongle-S3** (ESP32-S3 + 0.96″ ST7735 LCD).

This is a port of the [pico-usb-wifi](https://gitlab.com/baiyibai/pico-usb-wifi) design to ESP32-S3 USB-OTG: the dongle enumerates as a **USB CDC-NCM** Ethernet gadget, associates to Wi-Fi as a station, and transparently bridges Ethernet frames so the host appears on the AP’s LAN with a single MAC/IP identity.

```
┌────────────┐   USB CDC-NCM    ┌─────────────────────┐   802.11    ┌──────────┐
│ Host OS    │◄───────────────►│ T-Dongle-S3 firmware │◄──────────►│ Wi-Fi AP │
│ (DHCP/IP)  │   CDC-ACM mgmt  │  L2 bridge + LCD     │  STA assoc │          │
└────────────┘                 └─────────────────────┘             └──────────┘
```

No host-side Wi-Fi stack, vendor driver, or `wpa_supplicant` is required — only in-box `cdc_ncm` / `cdc_acm` class drivers (Linux, macOS, Windows 10+).

## Features

- Transparent **layer-2** USB ↔ Wi-Fi bridge (MAC adoption, no NAT)
- USB **CDC-NCM** network interface + **CDC-ACM** management console
- Up to 8 saved Wi-Fi credential profiles in NVS
- **SoftAP captive portal** when unconfigured: scans nearby SSIDs, broadcasts `ESP_WIFITOUSB_CONF`, config UI at `http://192.168.1.1`
- Site scan / join from the serial console (alternate provisioning path)
- **ST7735 LCD** live status: link state, SSID, RSSI, download/upload totals and rates
- **APA102** status LED (association / error patterns)
- Built with ESP-IDF 5.4 (+ IDF 6.x API patches) and Espressif TinyUSB (`esp_tinyusb`)
- No mandatory PSRAM — boots on T-Dongle-S3 revisions without working OPI RAM
- **BOOT button**: press once for reset prompt, again within 5 s to clear Wi‑Fi settings

## Quick start

### 1. Toolchain

```bash
git clone -b v5.4.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3 && . ./export.sh
```

### 2. Build

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
```

Optional compile-time Wi-Fi seed:

```bash
idf.py menuconfig   # USB Wi-Fi Bridge Configuration → Default WiFi SSID/Password
```

### 3. Flash (T-Dongle-S3)

The USB-A plug is the only port. While TinyUSB OTG is running, the Serial/JTAG CDC port disappears — enter ROM download mode to flash:

1. Hold the **BOOT** button
2. Plug the dongle into USB (or tap reset while holding BOOT)
3. Release BOOT
4. Flash:

```bash
idf.py -p /dev/ttyACM0 flash   # port name varies
```

5. Unplug and replug (without holding BOOT) to run the firmware

### 4. Provision Wi-Fi

**Option A — SoftAP captive portal (default when no SSID is saved)**

1. The stick scans nearby networks, then broadcasts open Wi‑Fi **`ESP_WIFITOUSB_CONF`**
2. Join that network from a phone/laptop
3. Open **`http://192.168.1.1`** (captive portal / DNS redirect)
4. Pick an SSID, enter the password, tap **Test & Save**
5. On a successful association test, credentials are stored in NVS and the setup SoftAP stops

**Option B — USB CDC console**

```text
picocom /dev/ttyACM0
(set|scan|list|use|del|save|status|help) # set ssid MyNetwork
(set|scan|list|use|del|save|status|help) # set pass hunter2
(set|scan|list|use|del|save|status|help) # save
```

Or `scan` → `join N` → `set pass …` → `save`.

### 5. Use the network

The host gets a USB Ethernet interface. NetworkManager / `systemd-networkd` / Windows / macOS usually DHCP automatically onto the AP’s subnet. The interface MAC matches the dongle’s Wi-Fi STA MAC.

## LCD status

| Field | Meaning |
|-------|---------|
| LINK | `CONNECTED` / `ASSOCIATING` / `SETUP AP` / `SCANNING` / `TESTING` / … |
| SSID / AP | Active network, or SoftAP name `ESP_WIFITOUSB_CONF` in setup mode |
| RSSI / URL | Signal when associated; portal IP `192.168.1.1` in setup mode |
| DN / STA | Download stats, or portal client / phase status in setup mode |
| UP / NET | Upload stats, or nearby-SSID count in setup mode |

## Documentation

| Doc | Contents |
|-----|----------|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How USB ethernet emulation + L2 bridging works |
| [docs/SETUP.md](docs/SETUP.md) | Full host OS setup, flashing, troubleshooting |
| [docs/HARDWARE.md](docs/HARDWARE.md) | T-Dongle-S3 pinout, USB-OTG, LCD, LED |
| [docs/RESEARCH.md](docs/RESEARCH.md) | Research notes: pico-usb-wifi, Espressif, LilyGO projects |

## Project layout

```
firmware/                 ESP-IDF application
  main/                   Bridge, Wi-Fi, SoftAP portal, console, LCD, LED
  components/esp_lcd_st7735/   LilyGO-proven ST7735 panel driver
  components/dns_server/       Captive-portal DNS redirect
docs/                     Architecture and setup guides
```

## Licence

MIT — see [LICENSE](LICENSE). Upstream components retain their own licences (Espressif ESP-IDF Apache-2.0, TinyUSB MIT, LilyGO ST7735 driver MIT).

## Credits / prior art

- [baiyibai/pico-usb-wifi](https://gitlab.com/baiyibai/pico-usb-wifi) — original L2 CDC-NCM bridge concept on Pico W
- [Espressif `tusb_ncm`](https://github.com/espressif/esp-idf/tree/v5.4.2/examples/peripherals/usb/device/tusb_ncm) — ESP32-S3 USB NCM + `esp_wifi_internal_tx` path
- [Espressif `usb_dongle`](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/device/usb_dongle) — ECM/RNDIS + CDC composite patterns
- [Espressif `sta2eth`](https://github.com/espressif/esp-idf/tree/v5.5.3/examples/network/sta2eth) — Wi-Fi STA ↔ wired/USB L2 forwarder
- [Xinyuan-LilyGO/T-Dongle-S3](https://github.com/Xinyuan-LilyGO/T-Dongle-S3) — board examples (LCD, USB-OTG TinyUSB mode)
