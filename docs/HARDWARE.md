# Hardware — LilyGO T-Dongle-S3

## Board overview

The T-Dongle-S3 packs an ESP32-S3 into a USB Type-A stick with an optional 0.96″ colour LCD.

| Item | Spec |
|------|------|
| SoC | ESP32-S3 dual-core Xtensa LX7 @ 240 MHz |
| Wireless | Wi-Fi 802.11 b/g/n (2.4 GHz), Bluetooth 5 LE |
| Flash | 16 MB (QIO) |
| PSRAM | 8 MB OPI on many LCD units (verify your revision) |
| Display | ST7735 IPS 80×160, 65k colour, 4-wire SPI |
| LED | APA102 RGB |
| Storage | TF / microSD slot inside the USB-A shell (SDMMC) |
| USB | USB-A plug → ESP32-S3 USB-OTG PHY |
| Antenna | PCB + optional IPEX (0 Ω resistor select) |
| Button | BOOT on GPIO0 |
| Form factor | ~58 × 18 × 9 mm |

Official sources:

- [LilyGO wiki — T-Dongle-S3](https://wiki.lilygo.cc/products/t-dongle-series/t-dongle-s3/)
- [GitHub Xinyuan-LilyGO/T-Dongle-S3](https://github.com/Xinyuan-LilyGO/T-Dongle-S3)
- [Zephyr board doc](https://docs.zephyrproject.org/latest/boards/lilygo/tdongle_s3/doc/index.html)

## Why this board fits the pico-usb-wifi port

1. **Native USB-OTG** on the stick’s only connector — ideal for a gadget that *is* a USB Wi-Fi dongle  
2. **Wi-Fi STA** on the same MCU that runs TinyUSB  
3. **On-board LCD** for link + traffic telemetry without a host UI  
4. Mature community examples for ST7735, APA102, SDMMC, and USB-OTG TinyUSB mode  

## Pin map used by this firmware

### ST7735 LCD

| Signal | GPIO | Notes |
|--------|------|-------|
| MOSI | 3 | SPI2 |
| SCLK | 5 | SPI2 |
| CS | 4 | |
| DC | 2 | |
| RST | 1 | |
| Backlight | 38 | **Active-low** (LEDC PWM) |

Panel quirks (from LilyGO `examples/lcd`):

- Colour order: **BGR**, inversion **on**
- Landscape 160×80: `swap_xy=true`, gap `(1, 26)`, mirror `(false, true)`
- Portrait alternative: gap `(26, 1)`, `swap_xy=false`

### APA102 LED

| Signal | GPIO |
|--------|------|
| Clock | 39 |
| Data | 40 |

### USB-OTG (fixed on ESP32-S3)

| Signal | GPIO |
|--------|------|
| D+ | 20 |
| D− | 19 |

Hard-wired to the USB-A plug. No external USB PHY.

### BOOT button

| Signal | GPIO | Notes |
|--------|------|-------|
| BOOT | 0 | Active-low, internal pull-up |

Firmware use (after boot): press once to show a **RESET WIFI?** confirm screen; press again within **5 seconds** to wipe saved Wi‑Fi profiles from NVS and re-enter SoftAP setup. If the second press does not arrive in time, the status HUD returns to normal.

### TF card (unused here; documented for completeness)

| Signal | GPIO |
|--------|------|
| CLK | 12 |
| CMD | 16 |
| D0 | 14 |
| D1 | 17 |
| D2 | 21 |
| D3 | 18 |

## USB-OTG vs USB-Serial/JTAG

ESP32-S3 has two USB controllers but **one** internal FS PHY:

| Mode | Used for | Appears as |
|------|----------|------------|
| USB-Serial/JTAG | ROM download, `idf.py monitor` when firmware uses it | CDC-like COM port |
| USB-OTG (TinyUSB) | NCM + ACM gadget (this firmware) | Network + serial composite |

LilyGO Arduino examples that need gadget mode require:

```
Tools → USB Mode → USB-OTG (TinyUSB)
```

and reject `ARDUINO_USB_MODE == 1` (Serial/JTAG). This ESP-IDF project uses OTG exclusively via `esp_tinyusb`.

### Practical flashing loop

1. BOOT-hold plug → download mode (Serial/JTAG) → `idf.py flash`  
2. Replug normally → OTG gadget runs  

You cannot reliably use `idf.py monitor` on the same ACM port while the NCM gadget is active; use the firmware’s CDC-ACM console instead.

## Power

Bus-powered from USB-A VBUS. There is also an MX 1.25 mm battery connector + charger on the PCB for tethered/battery experiments; this firmware assumes USB host power.

## Antenna

Default: PCB antenna. Some units expose IPEX; moving a 0 Ω resistor selects the external antenna (see LilyGO schematic). For a USB stick jammed in a metal chassis, external antenna can improve association reliability.

## Variants

| SKU / name | LCD | Notes |
|------------|-----|-------|
| T-Dongle-S3 with LCD | Yes | Primary target of this project |
| T-Dongle-S3 without LCD | No | Build with `CONFIG_BRIDGE_LCD_ENABLED=n` |
| T-Dongle-S3 Dual / Plus | Different | Different pinouts / extras — **not** validated here |

Firmware defaults leave SPIRAM **disabled** so sticks without reliable OPI PSRAM still boot. Confirm your board’s PSRAM and flash size against the sticker / LilyGO docs before enabling SPIRAM.

## How other projects use this board (summary)

| Project / example | USB | Wi-Fi | Display |
|-------------------|-----|-------|---------|
| LilyGO `lcd` / `factory_screen` | Serial CDC | Optional | ST7735 via `esp_lcd_st7735` |
| LilyGO `usb_hid_*` / `usb_mass_storage` | OTG TinyUSB | — | — |
| LilyGO `lvgl9` | — | — | LVGL on ST7735 |
| ESPHome T-Dongle-S3 profile | — | STA | ST7735 (`INITR_MINI160X80`), APA102 |
| Zephyr `tdongle_s3` | OTG / Serial-JTAG | Wi-Fi | Display support evolving |
| This firmware | OTG NCM+ACM | STA L2 bridge | Status HUD |

Cross-cutting lessons from those projects:

- Always use the LilyGO gap/mirror/invert sequence for the 80×160 panel  
- Backlight is active-low on GPIO38  
- USB gadget sketches must select OTG mode and expect BOOT-hold flashing  
- SDMMC pins collide with nothing we need for NCM+LCD+LED  
