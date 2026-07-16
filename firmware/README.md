# Firmware (ESP-IDF)

Build system entrypoint for the T-Dongle-S3 USB NCM ↔ Wi-Fi bridge.

```bash
. $IDF_PATH/export.sh   # ESP-IDF >= 5.1, tested on 5.4.2
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash    # PORT in download mode (hold BOOT while plugging)
```

See the repository root [README.md](../README.md) and [docs/SETUP.md](../docs/SETUP.md).
