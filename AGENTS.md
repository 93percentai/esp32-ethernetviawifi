# Agent notes

## Cursor Cloud specific instructions

This repo contains **ESP-IDF firmware** for two boards: the LilyGO T-Dongle-S3 USB network gadget (`firmware/`) and the ESP32-S31-Korvo-1 USB Ethernet host bridge (`firmware-korvo/`). There is no Node/Python app server, Docker Compose stack, or npm/pnpm workflow.

### ESP32-S31-Korvo-1

- Use ESP-IDF **v6.1**, tested commit `fff9895c82d744c7237be8847347bdd1b07c6643`.
- In the prepared workspace source `/workspace/setup/activate-idf61.sh`.
- From `firmware-korvo/`: `idf.py --preview set-target esp32s31`, then `idf.py --preview build size`.
- S31 is a RISC-V preview target, distinct from S3. Keep separate build directories and SDK environments.
- `sh tests/run.sh` runs host checks for network protocols, class/native initialization, USB lifecycle, forwarding, provisioning and log retention with ASan/UBSan.
- The stock LCD uses the official BSP; its backlight control APIs are unsupported.
- Native IDs, protocol limits and provenance are in [docs/USB_ADAPTERS.md](docs/USB_ADAPTERS.md). Prefer class-mode fallback over vendor initialization and do not guess register compatibility from a product name.
- USB host is installed by this application to provide a configuration-selection callback. Do not also call `bsp_usb_host_start()`.
- No physical hardware is attached in Cloud. Cross-compilation and mocked host checks do not establish USB, radio, LCD, DHCP or throughput operation.
- See [docs/KORVO_S31.md](docs/KORVO_S31.md) for adapter requirements and hardware validation.

The notes below apply to the original **LilyGO** build.

### Toolchain

- Use **ESP-IDF v6.x** (environment is set up with **v6.0.2** at `~/esp/esp-idf`).
- Before any `idf.py` command: `. ~/esp/esp-idf/export.sh` (or rely on the shell profile hook if already sourced).
- Target: `esp32s3` only. Standard commands are in [README.md](README.md) and [docs/SETUP.md](docs/SETUP.md).

### Build / check / run

| Action | Command (from `firmware/`) |
|--------|----------------------------|
| Configure + build | `idf.py set-target esp32s3` then `idf.py build` |
| Memory / “lint-like” check | `idf.py size` (no ESLint/pytest suite in-repo) |
| QEMU smoke boot | `idf.py qemu --qemu-extra-args "-nographic"` (needs `qemu-xtensa` + `libslirp0`) |
| Flash hardware | Hold **BOOT**, plug USB, then `idf.py -p /dev/ttyACM0 flash` |

### Gotchas

- **No physical T-Dongle-S3 in Cloud VMs**: full Wi-Fi/USB-NCM E2E cannot run here. Prove changes with `idf.py build` + optional QEMU boot logs.
- **QEMU limits**: ST7735 SPI init can hang without LCD hardware; disable LCD/LED in local `sdkconfig` (`CONFIG_BRIDGE_LCD_ENABLED` / `CONFIG_BRIDGE_STATUS_LED`) for a longer smoke path. Wi-Fi PHY calibration then stalls (no radio) — expected.
- **`firmware/sdkconfig` is gitignored**: regenerate via `idf.py set-target esp32s3`. After changing `sdkconfig.defaults*`, delete `sdkconfig` and rebuild (`idf.py fullclean` if needed).
- **`firmware/managed_components/` is gitignored**: Component Manager fetches `espressif/esp_tinyusb` on first configure/build.
- **SPIRAM stays off** by default (`sdkconfig.defaults.esp32s3`); do not force Octal PSRAM unless targeting a known-good board.
- Host flash/monitor needs download mode because TinyUSB owns the USB-OTG PHY (see [docs/SETUP.md](docs/SETUP.md)).
