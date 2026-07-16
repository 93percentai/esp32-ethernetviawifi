# Setup guide

## Hardware

- **LilyGO T-Dongle-S3 with LCD** (product codes often labelled K193 / “With LCD”)
- Host USB-A port (or A↔C adapter)
- 2.4 GHz Wi-Fi access point (ESP32-S3 has no 5 GHz radio)

See [HARDWARE.md](HARDWARE.md) for pinout and USB-OTG details.

## Install ESP-IDF

Tested with **ESP-IDF v5.4.2** (matches the `esp_tinyusb` 1.7.x APIs used in-tree).

```bash
git clone -b v5.4.2 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh esp32s3
. ./export.sh
```

Confirm:

```bash
idf.py --version
```

## Build the firmware

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
```

Artifacts:

- `build/esp32-ethernetviawifi.bin`
- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`

### Optional: bake default credentials

```bash
idf.py menuconfig
# → USB Wi-Fi Bridge Configuration
#    Default WiFi SSID
#    Default WiFi Password
```

You can still override/add profiles at runtime over the CDC console.

### Optional: disable LCD / LED

Same menuconfig section: `Enable ST7735 status LCD`, `Enable APA102 status LED`. Useful for the no-screen T-Dongle variant.

## Flashing the T-Dongle-S3

### Why BOOT-hold is required

The dongle’s USB-A connector is wired to the ESP32-S3 **USB-OTG** pins. This firmware runs TinyUSB in device mode on that PHY. USB-Serial/JTAG (the usual “CDC on boot” COM port) **shares the same PHY** and is unavailable while the gadget is running.

LilyGO’s own USB HID / MSC examples document the same requirement: Tools → USB Mode → **USB-OTG (TinyUSB)**, and flash via download mode.

### Enter download mode

1. Unplug the dongle  
2. Hold **BOOT**  
3. Plug into the host (keep holding)  
4. Release **BOOT**  
5. A serial/JTAG or USB-serial device should appear (Linux: often `/dev/ttyACM0`)

### Flash

```bash
. ~/esp/esp-idf/export.sh
cd firmware
idf.py -p /dev/ttyACM0 flash
```

Windows: use the COM port shown in Device Manager.  
macOS: `/dev/cu.usbmodem*` or similar.

### Run

Unplug, then plug **without** holding BOOT. The device enumerates as a composite CDC-NCM + CDC-ACM gadget.

## Host setup

### Linux

Modules (usually built-in on modern kernels):

```
cdc_ncm
cdc_ether   # sometimes pulled in
cdc_acm
```

Check:

```bash
dmesg -w
# expect something like: cdc_ncm ... register usb0 / enx...
ip link
ls /dev/ttyACM*
```

NetworkManager typically configures DHCP automatically. Manual:

```bash
sudo dhclient usb0          # interface name varies
# or
sudo dhcpcd enxXXXXXXXXXXXX
```

Management console:

```bash
picocom -b 115200 /dev/ttyACM0
# baud is irrelevant for USB CDC
```

If two ACM ports appear, try both; NCM + ACM ordering can vary by OS.

### macOS

CDC-NCM appears as a USB Ethernet interface in System Settings → Network. DHCP is usually automatic.

Serial console: `screen /dev/cu.usbmodem* 115200` or Serial.app.

### Windows 10 / 11

NCM should install as a USB Ethernet adapter via in-box drivers. The CDC-ACM port appears as a COM port — use PuTTY, Tera Term, etc.

If NCM does not enumerate, confirm you did **not** build with ECM-only / RNDIS-only; this project defaults to NCM.

### Embedded Linux (custom kernels)

Enable:

```
Device Drivers → Network device support → USB Network Adapters
  → Multi-purpose USB Networking Framework
     → CDC NCM support
Device Drivers → USB support → USB Modem (CDC ACM) support
```

## Provisioning over the console

On first boot (no NVS profiles, no baked SSID) the LCD shows `NO CONFIG` and the LED fast-blinks cyan.

```text
-- esp32-ethernetviawifi --
  profiles:   0 saved (active: none)
  ssid:       (unset)
  status:     idle (no credentials)
(set|scan|list|use|del|save|status|help) #
```

### Direct set

```text
set ssid MyNetwork
set pass hunter2
save
```

### Scan and join

```text
scan
join 1
set pass hunter2
save
```

### Multiple profiles

```text
list
use 2
del 1
save
```

Commands are case-insensitive. `status` reprints link + traffic counters. `resetstats` clears byte/frame totals.

## Verifying the bridge

1. LCD: `LINK CONNECTED`, SSID and RSSI populated  
2. Host interface has an IP from the AP (same subnet as other LAN clients)  
3. Host MAC equals the STA MAC printed on the console (`mac: aa:bb:…`)  
4. Ping the AP gateway; generate traffic and watch **DN** / **UP** rates on the LCD  

```bash
# Linux example
ip addr show
ping -c 3 1.1.1.1
iperf3 -c <server>    # optional throughput check
```

Expect USB Full-Speed class throughput on the order of a few Mbit/s of TCP payload (same class of limit as pico-usb-wifi). The Wi-Fi radio is not the bottleneck.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| No serial port after flash | Still in download mode / wrong cable | Replug without BOOT; try another port |
| Can’t flash while firmware runs | OTG owns the PHY | Hold BOOT while plugging |
| NCM interface missing | Host missing `cdc_ncm` | Load module / enable in kernel |
| Associates then no IP | Host DHCP not running | Check NM/`dhcpcd`; confirm AP DHCP |
| `BAD AUTH` on LCD | Wrong passphrase / WPA3-only quirks | Recheck password; try WPA2 AP |
| `NO AP` | 5 GHz-only SSID / out of range | Use 2.4 GHz SSID |
| Traffic one-way / stalls | USB power / hub issues | Plug into root port; avoid unpowered hubs |
| LCD blank | No-screen board variant / BL pin | Disable LCD in menuconfig; check `CONFIG_BRIDGE_LCD_ENABLED` |
| Build fails on PSRAM | Board without OPI PSRAM | `idf.py menuconfig` → disable SPIRAM, or use `sdkconfig` without `CONFIG_SPIRAM` |

### PSRAM note

`sdkconfig.defaults.esp32s3` enables Octal PSRAM (common on LCD units). If your stick has no PSRAM, remove or comment those `CONFIG_SPIRAM*` lines and reconfigure:

```bash
rm sdkconfig
idf.py set-target esp32s3
idf.py build
```

## Updating firmware without losing Wi-Fi profiles

NVS occupies its own partition; a normal `idf.py flash` of the app image keeps saved profiles. Erasing flash (`idf.py erase-flash`) clears them.

## Development tips

- After changing Kconfig defaults, delete `sdkconfig` or run `idf.py fullclean` before rebuilding.  
- `idf.py size` shows IRAM/DRAM usage.  
- For host-side packet capture: `tcpdump -i <usbif> -n`.  
- Console and NCM share the same Full-Speed USB pipe — avoid flooding the console during throughput tests.
