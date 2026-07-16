# Research notes

Background gathered while porting [pico-usb-wifi](https://gitlab.com/baiyibai/pico-usb-wifi) to the LilyGO T-Dongle-S3.

## 1. pico-usb-wifi (source of the design)

Repository: https://gitlab.com/baiyibai/pico-usb-wifi (MIT)

**What it is:** Raspberry Pi Pico W firmware that enumerates as USB CDC-NCM (+ dual CDC-ACM) and bridges Ethernet frames to the CYW43439 Wi-Fi station.

**Key design choices reused here:**

- Transparent L2 bridge, not NAT/tether
- Host adopts the Wi-Fi STA MAC (single identity)
- Self-reflection filter for AP-flooded copies of the host’s own broadcasts
- Out-of-band CDC-ACM provisioning (works before association)
- Multi-profile credential storage + scan/join UX
- Explicit documentation of the “Wi-Fi STA cannot bridge multiple MACs” constraint

**Pico-specific pieces not carried over:**

- `cyw43_*` / `allmulti` iovar (Infineon-specific)
- Main-loop-only TinyUSB rule + deferred USB-TX ring (Pico concurrency model)
- Dual debug CDC port (LCD replaces periodic stats stream)
- UF2 / pico-sdk build

## 2. Espressif official USB ↔ Wi-Fi examples

### `examples/peripherals/usb/device/tusb_ncm` (ESP-IDF)

- Target: ESP32-S2/S3 (+ later P4 with remote Wi-Fi)
- Uses `esp_tinyusb` + `tinyusb_net`
- Sets NCM MAC from `esp_read_mac(..., ESP_MAC_WIFI_STA)` / `esp_wifi_get_mac`
- Forwards with `esp_wifi_internal_tx` and `esp_wifi_internal_reg_rxcb`
- **This is the primary ESP32 data-path template for the port**

IDF 5.4.2 API shape used by this project:

```c
tinyusb_driver_install(&tusb_cfg);
tinyusb_net_init(TINYUSB_USBDEV_0, &net_config);
esp_wifi_internal_tx(ESP_IF_WIFI_STA, buffer, len);
esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, pkt_wifi2usb);
```

### `esp-iot-solution` → `usb_dongle`

- Composite options: ECM/RNDIS + CDC + DFU (+ optional BTH)
- Documents OS matrix: Windows→RNDIS, macOS→ECM, Linux→both
- CLI over CDC/UART for `sta -s … -p …` and SmartConfig
- Endpoint budget warning: do not enable ECM+BTH+CDC all at once
- Validates that ESP32-S3 USB-A / DevKit wiring is D+=GPIO20, D−=GPIO19

### `examples/network/sta2eth`

- Generic Wi-Fi STA ↔ “wired” L2 forwarder
- Wired side can be Ethernet MAC **or** USB NCM (S2/S3)
- Adds provisioning SoftAP / webpage (`http://wifi.settings`) when unconfigured
- Confirms Espressif considers USB-NCM a first-class “wired” peer for STA bridging

### USB Device Stack docs

- TinyUSB packaged as managed component `espressif/esp_tinyusb`
- NCM selectable via `CONFIG_TINYUSB_NET_MODE_NCM`
- Default descriptors available for CDC / MSC / NCM; composite CDC+NCM enabled in this project’s `sdkconfig.defaults`

## 3. Ethernet-over-USB class landscape

| Standard | Role | Adoption |
|----------|------|----------|
| CDC-ECM | Early Ethernet gadget | macOS/Linux; not stock Windows |
| RNDIS | Microsoft Ethernet gadget | Windows; Linux OK; macOS no |
| **CDC-NCM** | Modern CDC networking | Linux, macOS, Windows 10+ |

pico-usb-wifi and Espressif `tusb_ncm` both settled on NCM for the same reason: one binary, three desktop OSes.

Other related projects surveyed:

- **USBCoercer** — ESP32 TinyUSB NCM gadget + embedded DHCP/WPAD (security research; different goal, same NCM base)
- **lrndis** / **usbnet** — historical RNDIS/NCM MCU libraries cited by pico-usb-wifi

## 4. LilyGO T-Dongle-S3 ecosystem

### Official repo examples that informed the port

| Example | Takeaway |
|---------|----------|
| `examples/lcd` | Canonical ST7735 init: BGR, invert, landscape gap `(1,26)`, BL GPIO38 active-low |
| `examples/factory_screen` | Same panel driver used in shipping demo |
| `examples/usb_hid_keyboard` / `mouse` / `usb_mass_storage` | Must select **USB-OTG (TinyUSB)**; BOOT-hold flash loop |
| `examples/led` | APA102 on GPIO39/40 |
| `boards/dongles3.json` | 16 MB flash, USB VID/PID hints, PlatformIO flags |

### Community / OS support

- **ESPHome** device profile: ST7735 `INITR_MINI160X80`, dual SPI buses (LCD + APA102), LEDC backlight inverted
- **Zephyr** `lilygo/tdongle_s3`: documents USB-OTG, Wi-Fi, APA102; useful for pin confirmation
- **PlatformIO community threads**: OTG vs CDC-on-boot confusion; confirms Serial/JTAG disappears in OTG mode

### USB + Wi-Fi coexistence notes

Forum/docs occasionally mention RF / PHY interactions when USB is active. In practice Espressif ships USB-NCM+Wi-Fi examples as supported; keep USB cable short and avoid unpowered hubs if association is flaky.

## 5. Design decisions for this port

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Framework | ESP-IDF 5.4 + `esp_tinyusb` | First-class NCM + Wi-Fi internal TX/RX |
| Network class | CDC-NCM only (not RNDIS/ECM) | Match pico-usb-wifi + best host coverage |
| Console | Single CDC-ACM | Endpoint budget + LCD for live stats |
| Provisioning | Serial commands + NVS profiles | Parity with pico-usb-wifi UX |
| Display | Direct `esp_lcd` + LilyGO ST7735 driver | Proven on this panel; no LVGL overhead |
| Bridging | Raw L2 (`esp_wifi_internal_*`) | Same semantics as pico / `tusb_ncm` |
| Not chosen | SoftAP captive portal (`sta2eth`) | Serial console is enough for a USB stick |

## 6. Known gaps / future work

- IPv6 multicast completeness vs pico’s `allmulti` (may need explicit multicast filter API work)
- Second CDC debug stream (optional)
- Throughput tuning (NTB sizes, Wi-Fi AMPDU, pinned cores) — USB FS remains the ceiling
- Captive SoftAP fallback for headless provisioning without a serial terminal
- Validate on no-LCD / Dual / Plus board variants
- Encrypted NVS for credential-at-rest

## 7. References

1. https://gitlab.com/baiyibai/pico-usb-wifi  
2. https://github.com/espressif/esp-idf/tree/v5.4.2/examples/peripherals/usb/device/tusb_ncm  
3. https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/device/usb_dongle  
4. https://github.com/espressif/esp-idf/tree/v5.5.3/examples/network/sta2eth  
5. https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_device.html  
6. https://github.com/Xinyuan-LilyGO/T-Dongle-S3  
7. https://wiki.lilygo.cc/products/t-dongle-series/t-dongle-s3/  
8. https://docs.zephyrproject.org/latest/boards/lilygo/tdongle_s3/doc/index.html  
9. https://github.com/esphome/esphome-devices (Lilygo-TDongle-S3 profile)  
