# Agent notes

## Cursor Cloud specific instructions

This repo is **ESP-IDF firmware** (LilyGO T-Dongle-S3 USB CDC-NCM ↔ Wi-Fi bridge). There is no Node/Python app server, Docker Compose stack, or npm/pnpm workflow.

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
