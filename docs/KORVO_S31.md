# ESP32-S31-Korvo-1 Wi-Fi to Ethernet bridge

This build makes a **single directly connected Ethernet device** a client of
your Wi-Fi router. Connect the USB Ethernet adapter to the Korvo's **USB-A
USB 2.0 HS host port**, then connect its RJ45 socket to the device. The S31
is a RISC-V preview target, distinct from ESP32-S3. The original LilyGO USB
network gadget remains in `firmware/`; this application is `firmware-korvo/`.

```text
Wi-Fi router <--802.11--> S31-Korvo-1 <--USB CDC ECM--> adapter <--RJ45--> one client
        DHCP/IP assigned directly to the wired client (no NAT)
```

## Adapter requirements

The host driver implements **CDC ECM**, communication interface class `02`,
subclass `06`, protocol `00`, a CDC Union descriptor, an Ethernet descriptor,
an interrupt-IN endpoint, and a CDC Data interface with bulk-IN/bulk-OUT.
It follows the Union's actual interface numbers and the advertised alternate
setting; they need not be adjacent or use alternate 1. The adapter must accept
`SET_ETHERNET_PACKET_FILTER` with promiscuous reception: replies addressed to
the wired client have a different MAC from the adapter's factory MAC.

**A USB-to-Ethernet product name alone does not establish compatibility.**
RNDIS, CDC NCM host mode, and vendor-specific ASIX/Realtek protocols are not
implemented. Some adapter revisions expose ECM in another USB configuration;
others expose only vendor interfaces. Do not assume every RTL8152/RTL8153 or
AX88772/AX88179 adapter works. No particular adapter has been physically tested
with this firmware, and the user's adapter model has not yet been established.

Use your [USBView firmware](https://github.com/93percentai/esp32-usbview) to inspect
the VID:PID, configuration values and interfaces. In `idf.py --preview menuconfig`
under **Korvo Wi-Fi Ethernet bridge**, set **USB bConfigurationValue to select**
to the configuration advertising ECM (default `1`; sometimes `2`). The selection
applies at enumeration, before interfaces are claimed. Values must match the
device's descriptors. An unavailable configuration fails host enumeration;
serial logs show that failure. Vendor-only adapters need a separate chipset driver.

The port has a hardware 500 mA current limit. Use an adequate board supply;
adapters needing more power require a separately powered high-speed hub.
The pinned host stack supports hubs but lacks full-/low-speed transaction
translation behind a high-speed hub. Prefer connecting the adapter directly.
USB 3 adapters must enumerate in a supported USB 2 mode.

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
- Two USB receive transfers preserve completion order and frame boundaries.
  Exact-max-packet-size USB writes include a terminating zero-length packet.
  Wi-Fi receive buffers are copied to a bounded 24-frame queue and released
  once; USB never retains a borrowed Wi-Fi buffer. Counters distinguish queued
  frames from completed USB transmissions.
- USB/cable disconnect and Wi-Fi-down events invalidate queued transmissions.
  Pending USB callbacks are drained before interfaces/device handles are closed.
  A stalled control transfer (2 s) or transmit (500 ms) resets the USB host port.
  Other USB errors stop forwarding and ask for an adapter reconnect.
- Adapter enumeration is not Ethernet link establishment. A CDC cable
  notification or a valid received Ethernet frame establishes the link.
  No Wi-Fi/USB throughput or latency claim is made before hardware measurements.

## Validation

```sh
cd /workspace/esp32-ethernetviawifi
sh tests/run.sh
cd firmware-korvo
idf.py --preview build size
```

Host tests use ASan/UBSan. They cover malformed/truncated CDC descriptors,
nonadjacent interfaces and alternate settings, strict MAC strings, 30,000 random
inputs, frame bounds, Wi-Fi form decoding/escaping, and the actual USB driver
with mocked host APIs: setup requests, promiscuous filter, frame completion
order, queue overflow, stale generations, unplug while URBs are pending and
replug. Forwarding tests exercise actual bridge callbacks: one-client learning,
MAC-adoption gating, foreign sources/destinations, reflection drops, queue
failure and exactly-once Wi-Fi buffer release. Mocks do not simulate a USB
controller, Wi-Fi radio, DMA/cache behavior, FreeRTOS concurrency or LCD.

Cloud validation on 2026-11-14 passed all four host test executables, the
S31 v6.1 build/size check with hub support enabled, and the original S3
v6.0.2 build/size regression check. The S31 image-size report is approximately
1.53 MB against its 6 MB application partition; this is not a runtime heap
measurement. The SDK emits upstream private-include dependency warnings;
the application compiles and links. No physical serial device is attached.

Before calling a board/adapter combination working, record its VID:PID and
configuration and perform these physical checks:

- Confirm boot, LCD/touch, HS enumeration, ECM claims/filter and cable events.
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
[MS2109 project](https://github.com/93percentai/esp32-MS2109). S31 board defaults,
USB HS and display initialization were checked against that project and USBView.
No USBView GPL database or application source is included. The BSP/USB/LVGL
components retain their respective upstream licenses. CDC ECM parsing and
forwarding in this application are project code under this repository's MIT
license. ESP-IDF's `sta2eth` example is architectural reference, not a USB host
adapter driver.
