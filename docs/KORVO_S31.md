# ESP32-S31-Korvo-1 Wi-Fi to Ethernet bridge

This build makes a **single directly connected Ethernet device** a client of
your Wi-Fi router. Connect the USB Ethernet adapter to the Korvo's **USB-A
USB 2.0 HS host port**, then connect its RJ45 socket to the device. The S31
is a RISC-V preview target, distinct from ESP32-S3. The original LilyGO USB
network gadget remains in `firmware/`; this application is `firmware-korvo/`.

```text
Wi-Fi router <--802.11--> S31-Korvo-1 <--USB Ethernet--> adapter <--RJ45--> one client
        DHCP/IP assigned directly to the wired client (no NAT)
```

## Adapter support

The firmware supports **CDC ECM, CDC NCM16 and RNDIS**, plus native drivers
for **AX88772/A/B**, **AX88179/AX88178A**, **RTL8152**, **DM96xx** and
**LAN95xx** adapter families. Standard-class discovery is independent of retail
brand and VID:PID. Native register commands are restricted to the exact chipset
and OEM IDs in the registry; native RTL8152 also checks its silicon revision.

By default the application inspects up to **16 USB configurations**, prefers
ECM, then NCM, RNDIS and native mode, and re-enumerates in the selected
configuration. This handles adapters whose default configuration contains only
vendor interfaces or a driver CD image. No storage is mounted or executed. If
initialization fails, it tries the next compatible configuration once. USB
unplug clears the failure history so reconnection can retry. Re-enumeration
resets the root port and all downstream USB devices. The RTL8153 profile
`0bda:8153`, `bcdDevice=3100` with two configurations selects its advertised
ECM configuration 2 on initial enumeration, avoiding that reset. For other
configuration changes, recovery waits for the full device tree (including hubs)
to be released before restarting; serial and LCD diagnostics report the phase,
root-port errors and reset failure count.

See [the adapter matrix and troubleshooting guide](USB_ADAPTERS.md) for native
IDs, protocol limits, firmware options and the difference between software
coverage and physical validation. **No adapter/board combination has been
physically qualified by these Cloud checks.** A name such as “RTL8153” or
“AX88179” is not a guarantee for every product revision.

Your [USBView firmware](https://github.com/93percentai/esp32-usbview) remains
useful for inspecting VID:PID, configurations, interfaces and negotiated speed.
For manual selection, disable **Discover Ethernet configurations automatically**
in `idf.py --preview menuconfig` → **Korvo Wi-Fi Ethernet bridge**, then set
**USB bConfigurationValue to select**. The host library requires configuration
value = descriptor index + 1; unusual nonconsecutive values are reported.

The USB-A port has a hardware 500 mA current limit. Use an adequate board supply;
adapters needing more power require a separately powered hub. USB 3 adapters
must enumerate in USB 2 compatibility mode. The pinned host stack lacks
full-/low-speed transaction translation behind a high-speed hub. Connect those
adapters directly, or try **Force USB full-speed host mode** in menuconfig.
The latter limits the entire root bus to 12 Mbit/s.

## Build and flash

Use **ESP-IDF v6.1** with S31 preview support. Tested SDK commit:
`fff9895c82d744c7237be8847347bdd1b07c6643`. The pinned component lock includes
Korvo BSP `1.0.1`, USB host `1.6.0` and LVGL `9.5.0`.

```sh
# Prepared cloud workspace; elsewhere source your S31-capable IDF export.sh.
. /workspace/setup/activate-idf61.sh
cd /workspace/esp32-ethernetviawifi/firmware-korvo
idf.py --preview set-target esp32s31
idf.py --preview build size
idf.py --preview -p /dev/ttyUSB0 flash monitor
```

Use the **UART USB-C** port to flash/debug; choose your actual serial port.
The other USB-C connector is power-only. If necessary, hold BOOT, press/release
RESET, then release BOOT to enter download mode. Keep the LCD subboard attached
for display/touch. Its unsupported backlight-control APIs are deliberately not
called. LCD initialization failure is logged without stopping the bridge.
For operation without the LCD, disable **Enable stock 800x480 touch display**.

Defaults select 16 MB flash, 250 MHz PSRAM, a 6 MB application partition and
4096-byte USB enumeration transfers, following your S31 display/USB work.
Defaults apply to a newly generated `sdkconfig`. After changing defaults,
regenerate it with `set-target`, or apply the settings through menuconfig.
Wi-Fi credentials may optionally be seeded in menuconfig; leave them blank
to configure at runtime. Do not commit credentials or a personal `sdkconfig`.

## Connect and configure

1. Connect the Ethernet adapter and one client. The client should use DHCP.
2. With no saved Wi-Fi profile, join **ESP_WIFITOUSB_CONF** from a phone or PC.
3. Open **http://192.168.1.1**, choose or type an SSID, enter its password, and
   select **Test & Save**. Open networks use a blank password.
4. Successful association and NVS saving close the setup AP. As in the original
   bridge, this tests Wi-Fi association, not internet reachability or upstream DHCP.
5. The first IPv4, ARP or IPv6 frame learns the wired client's source MAC.
   The S31 stops/restarts its station with that MAC and reconnects to Wi-Fi.
   Initial packets are dropped during that transition; DHCP normally retries.
   Renew DHCP on the client if needed. The client then gets a lease directly
   from the Wi-Fi router, with its own MAC identity and subnet.
6. The LCD shows adapter identity, cable state, client MAC, forwarding state,
   transfer totals/rates and drops. UART prints status every five seconds.
7. Select **Wi-Fi setup** on the touchscreen to change Wi-Fi. If saved Wi-Fi
   cannot connect for 30 seconds, setup opens automatically, including with
   the LCD disabled. Stored profiles remain until replaced or explicitly erased.
8. To use a different Ethernet device, unplug/replug the RJ45 cable or adapter.
   A notified cable-down/USB disconnect clears the learned client. Adapters
   without cable-down notifications require USB unplug/replug to relearn.

The setup AP is open and the HTTP portal follows the existing project's local
setup flow. It stops after a successful test/save. There is no management IP
on the bridged station; `192.168.1.1` is the setup AP only. The firmware creates
no STA IP stack or DHCP client that could compete with the wired device.

## Serial and touchscreen debugging

The LCD has **Overview**, **Diagnostics** and **Logs** tabs. Diagnostics and Logs
scroll vertically. Overview shows the selected driver/configuration, USB identity,
Wi-Fi state, link and client identity, traffic and current setup instruction.

Diagnostics includes endpoints and max-packet sizes, control/data interfaces and
alternate setting, USB speed, MAC addresses, silicon revision, last control
request and transfer status, initialization stage, queue saturation/stale drops,
malformed USB blocks, timeouts, DHCP/ARP activity in each direction, MAC-adoption
attempts/failures, Wi-Fi disconnect reason and free/minimum internal heap/PSRAM.
The USB speed is negotiated bus speed; Ethernet link rate is reported separately.
An unknown duplex/rate is not a throughput measurement.

UART logs configuration/interface/endpoint discovery, protocol selection,
re-enumeration/fallback, initialization results, cable changes, and complete
control/transfer failure details. Status summaries print every five seconds.
Enable **Log every USB control request to serial and screen log** in menuconfig
for request type/code, value/index, length and result. Packet payloads and Wi-Fi
passwords are not printed by this tracing.

The Logs tab retains the latest 16 KiB of ESP application logs, including host
stack enumeration errors that occur before a device can be opened. It shows
retention-overwrite and fragment-truncation counters and removes ANSI colors.
The visible tail is 4 KiB. UART output continues normally. Logs reset on reboot;
ROM/bootloader messages and direct `printf` output remain serial-only. Capture
UART from reset when diagnosing a startup crash.

## Forwarding and limits

- This is **one-client MAC adoption**, using the same raw Wi-Fi API concept as
  the original bridge. It is not a multi-client bridge, repeater or router.
  Connect one endpoint directly. A switch can cause the wrong source to be
  learned; subsequent sources are dropped. LLDP cannot select a client.
- The station adopts the **wired client's MAC**, not the USB adapter's MAC.
  Both directions retain Ethernet/ARP/DHCP/IP contents unchanged. Reflection
  filtering prevents the AP returning the client's own broadcasts back to it.
- Frames use standard untagged Ethernet with MTU 1500 (14–1514 bytes without
  FCS). VLAN frames and jumbo frames are dropped. IPv4 and IPv6 frames are
  forwarded, but physical IPv6 operation has not been verified.
- Two USB receive transfers preserve completion order. ECM frame boundaries and
  NCM/RNDIS/native aggregate metadata are validated before any frame is delivered.
  Exact-max-packet-size USB writes include a terminating zero-length packet.
  Wi-Fi receive buffers are copied to a bounded 24-frame queue and released
  once; USB never retains a borrowed Wi-Fi buffer. Counters distinguish queued
  frames from completed USB transmissions.
- USB/cable disconnect and Wi-Fi-down events invalidate queued transmissions.
  Pending USB callbacks are drained before interfaces/device handles are closed.
  A stalled control transfer (2 s) or transmit (500 ms) resets the USB host port.
  Other USB errors stop forwarding and ask for an adapter reconnect.
- Adapter enumeration is not Ethernet link establishment. A CDC cable
  notification, a polled native/RNDIS link result, or a valid received Ethernet
  frame establishes the link.
  No Wi-Fi/USB throughput or latency claim is made before hardware measurements.

## Validation

```sh
cd /workspace/esp32-ethernetviawifi
sh tests/run.sh
cd firmware-korvo
idf.py --preview build size
```

Host tests use ASan/UBSan and strict compiler warnings. They exercise:

- Existing provisioning form bounds and escaping, and original ECM bounds.
- Generic ECM/NCM/RNDIS/native descriptor binding and exact native-ID matching.
- NCM16 alignment, multiple datagrams, NDP cycles, metadata/payload overlap;
  RNDIS message bounds; native length/CRC/padding and aggregate boundaries.
- More than 40,000 randomized descriptor/payload inputs, with no partial
  publication of a malformed aggregate.
- The actual USB manager with mocked host APIs: automatic selection, fallback
  after a rejected ECM filter, manual configuration, hotplug during pending
  URBs, stale/full queues, exact completion order, and a timed-out EP0 request.
- RNDIS INIT/query/filter request IDs and status, NCM negotiation and FS size
  limits, native initialization/link models, promiscuous mode and control-error
  propagation, plus rejecting unsupported RTL8153 native silicon.
- Actual bridge callbacks and Wi-Fi buffer ownership, plus log-ring wrapping,
  overwrite accounting and bounded snapshots.

The expanded firmware is cross-compiled for S31 with ESP-IDF v6.1. The original
S3 v6.0.2 build is checked for regressions from the shared Wi-Fi status changes.
Mocks do not model the controller, DMA/cache behavior, radio, physical PHY,
FreeRTOS scheduling, LCD or real adapter quirks. See the adapter matrix for the
physical status. No serial hardware is attached in Cloud.

Before calling a board/adapter combination working, record its VID:PID and
configuration and perform these physical checks:

- Confirm boot, LCD/touch, USB enumeration, driver initialization/filter and cable events.
- Provision a valid network, an invalid password and a hidden 32-byte SSID;
  verify reboot persistence and automatic setup fallback.
- Renew the wired client's DHCP lease. Confirm the router sees the client's
  MAC/IP and the S31 has no competing lease. Test gateway ping, DNS and internet.
- Test LAN TCP/UDP in both directions, broadcast discovery and IPv6 if needed.
  Measure throughput and drops rather than assuming USB HS equals link speed.
- Unplug/replug USB and RJ45 during traffic; switch Ethernet clients, restart
  the AP and recover Wi-Fi after an outage. Check memory stability and counters.
- Leave it under sustained traffic, with display refresh and touch setup, and
  test a slow/stalled USB adapter and queue saturation.

Cloud builds cannot perform those physical checks.

## Reused sources

Wi-Fi profiles, provisioning and captive DNS are shared with the LilyGO build.
Strict form helpers are reused from your MIT-licensed
[MS2109 project](https://github.com/93percentai/esp32-MS2109). The log-ring helpers also come from that project. S31 board defaults,
USB HS and display initialization were checked against that project and USBView.
No USBView GPL database or application source is included. The BSP/USB/LVGL
components retain their respective upstream licenses. The RTL8152 initializer is adapted from Apache-2.0 CherryUSB; its copyright,
license, upstream commit and port changes are retained in `main/vendor/`.
The AX88772 reset sequence was also checked against CherryUSB. See
[adapter research and provenance](USB_ADAPTERS.md#sources-and-provenance). ESP-IDF's `sta2eth` example is architectural reference, not a USB host
adapter driver.
