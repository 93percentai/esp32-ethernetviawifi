# USB Ethernet adapters and diagnostics

This matrix describes **implemented software paths**, not a list of physical
adapter certifications. All paths cross-compile for ESP32-S31 and have host
protocol or register-model checks; none has been tested on a physical Korvo
board in this task. USB 3 devices require USB 2 backward-compatible operation.
One USB NIC and one directly attached Ethernet client are supported at a time.

## Implemented paths

| Protocol / family | How it is matched | Scope |
| --- | --- | --- |
| CDC ECM | Communication interface `02/06/00`, CDC Union + Ethernet descriptors, bulk data pair | Brand-independent. This includes class-mode variants of Realtek, ASIX, WCH and other adapters when advertised. Promiscuous packet filter must work. |
| CDC NCM16 | Interface `02/0d/00`, Union + Ethernet/NCM descriptors, data pair | Negotiates NTB16, input size, divisor/remainder and NDP alignment. Supports aggregate RX and single-frame NTB TX. CRC mode disabled when selectable. NCM32-only devices are rejected. |
| RNDIS | `e0/01/03`, `ef/04/01`, or legacy `02/02/ff` with Union/two-interface IAD | INIT, MAC query, promiscuous receive filter, bounded response polling and media/rate queries. Tethering gadgets may expose this, but this project remains a transparent one-client bridge, not a cellular router. |
| ASIX AX88772/A/B | Exact chipset/OEM IDs below | Native USB 2 10/100 framing, PHY reset/autonegotiation, MII link/duplex polling, promiscuous RX and packet-boundary padding. |
| ASIX AX88179 / AX88178A | Exact chipset/OEM IDs below | Native gigabit-family register path and trailer metadata/aggregate framing, with two-byte RX alignment removed. RX aggregates limited to 2 KiB; checksum/TSO offload disabled. Prefer advertised ECM/NCM on newer revisions. |
| Realtek RTL8152 | `0bda:8152`, then silicon revision read | Native revisions **4c00, 4c10, 4800** only. Checks revision before initialization. Aggregation, VLAN stripping and RX checksum offload disabled; native RX descriptor and FCS removed. RTL8153/8156/8157 native initialization is not implemented. |
| Davicom DM96xx | Exact chipset/OEM IDs below | DM9601 framing; DM9620 mode bit switches to compatible framing. PHY/link polling, promiscuous RX, length/FCS checks and DM962x TX padding. |
| Microchip/SMSC LAN95xx | Exact chipset IDs below | Native LAN9500-family register protocol, PHY polling, promiscuous RX, checksum trailer disabled, single-frame RX. Includes the Ethernet function of LAN9512/9514 hubs (`0424:ec00`). |

The generic class paths have **no retail-brand whitelist**. For example,
RTL8153-family and AX88179A/B devices that advertise a usable ECM/NCM configuration
are handled by that protocol, not by an unrelated native initializer. New
production batches can change silicon, firmware, descriptors and power needs.
A driver being selected is not evidence that Ethernet forwarding works on that
particular unit.

NCM currently accepts NCM0 (no-CRC) NDP16 chains (up to 16 NDPs and 256 frames),
rejects cycles and overlapping
metadata/datagrams, and limits one block to 16 KiB (4 KiB input on USB full
speed). It does not implement MBIM, NCM32, or checksum offload. A NCM device
without an advertised packet-filter command must supply unrestricted default
reception for this bridge to work. RNDIS packet/status payloads are bounded and
matched to the request ID; unsupported per-packet metadata is rejected.

## Native ID registry

The source of truth is `usb_net_native_match()` in
[`firmware-korvo/main/usb_net.c`](../firmware-korvo/main/usb_net.c). The registry
uses exact IDs, and standard protocols always take priority during discovery.

| Native family | VID:PID values |
| --- | --- |
| AX88772/A/B | `0b95:7720`, `0b95:772a`, `0b95:772b`, `0b95:7e2b`, `05ac:1402`, `17ef:7203`, `13b1:0018`, `1557:7720`, `07d1:3c05`, `2001:3c05`, `2001:1a02`, `0db0:a877` |
| AX88179/178A-compatible core | `0b95:1790`, `0b95:178a`, `04b4:3610`, `2001:4a00`, `0df6:0072`, `04e8:a100`, `17ef:304b`, `050d:0128`, `0930:0a13`, `0711:0179`, `07c9:000e`, `07c9:000f`, `07c9:0010` |
| RTL8152 | `0bda:8152`, restricted by the revision check above |
| DM96xx | `07aa:9601`, `0a46:9601`, `0a46:6688`, `0a46:0268`, `0a46:8515`, `0a47:9601`, `0fe6:8101`, `0a46:9000`, `0a46:9620`, `0a46:9621`, `0a46:9622`, `0a46:0269`, `0a46:1269` |
| LAN95xx | `0424:9500`, `0424:9505`, `0424:9e00`, `0424:9e01`, `0424:ec00`, `0424:9530`, `0424:9730`, `0424:9e08`, and `0424:9900` through `0424:9909` |

OEM power/reset/PHY quirks can still require further work, even for a listed ID.
Drivers require a valid programmed MAC address. No EEPROM writes, firmware
uploads, driver-CD execution or mass-storage mode-switch commands are performed.
Chipsets such as LAN78xx, native RTL8153/8156/8157, older AX88172/AX88178,
SR9700/SR9900 and CH9200 require another native driver unless they advertise a
supported standard class. Their register protocols must not be guessed from a
similar product name.

## Configuration discovery and fallback

1. Enumerate configuration 1 and log cached identity/speed/product information.
2. Inspect the active configuration and read the other configuration descriptors
   with standard GET_DESCRIPTOR requests, up to 16 configurations and 4096 bytes
   per descriptor. Log every interface, alternate, endpoint and candidate.
3. Prefer **ECM → NCM16 → RNDIS → native**, retaining the current configuration
   on equal scores. This favors the simplest supported bridge mode.
4. If necessary, save the VID/PID/revision/configuration choice, reset the root
   port, drain pending URBs and re-enumerate with the chosen configuration.
5. Claim only the chosen interfaces, initialize the protocol and enable
   promiscuous reception where supported. Native PHY queries configure the MAC
   for the resolved link speed and duplex.
6. If initialization fails, mark that configuration tried and retry the next
   candidate. A normal unplug clears the tried set, including after an adapter
   has already been closed on an error. A planned re-enumeration retains it.

The pinned USB host maps configuration index to value using `index + 1`; sparse
nonconsecutive configuration values are reported rather than selected blindly.
An unsupported default interface alone no longer means the whole adapter is
unsupported. Devices that cannot enumerate at all remain a host/power/descriptor
problem before the adapter driver can run.

## Useful diagnostics

| Symptom | Check on screen / serial |
| --- | --- |
| Nothing enumerates | Logs tab for host enumeration errors; UART from reset; USB-A HS port, board supply, adapter power, a direct connection. Low/full-speed behind a HS hub needs transaction translation absent in this host version. |
| Native driver not selected | Logged class candidates in every configuration; class mode is preferred. Check exact ID and reported silicon revision. |
| Configurations keep changing | Fallback log with rejected protocol/configuration and failed EP0 type/code/value/index/status. Each candidate is tried once, not forever. |
| Filter/control STALL | Driver, last request and `last_error`. Enable control tracing. A driver must receive Ethernet frames addressed to MACs other than the adapter's own. |
| Cable up, no client learned | USB RX blocks/frames/bad counters. Renew DHCP or trigger ARP from the wired client. Native metadata/FCS must decode before learning can occur. |
| Learned client, bridge not forwarding | Wi-Fi state, RSSI/channel/disconnect reason, station/client MAC, adoption attempts/failures, and last Wi-Fi TX result. |
| No DHCP lease | DHCP Ethernet/Wi-Fi counters plus ARP counters. No upstream reply suggests Wi-Fi/router/receive-filter issues; malformed USB counts suggest a framing issue. These counters classify traffic, not prove a lease was granted. |
| Traffic drops | Queue high-water/full/stale counters, transfer status/endpoint, PHY rate/duplex, memory minimum and last RX/TX age. The 24-frame queue never retains a borrowed Wi-Fi buffer. |
| Error history appears after recovery | `last_error` is retained for diagnosis; current stage, ready/link and counters distinguish it from an active failure. |

Menuconfig → **Korvo Wi-Fi Ethernet bridge** offers:

- Automatic configuration discovery (default on).
- A manual configuration value when discovery is disabled.
- Logging of every control request (default off): headers and lengths/results,
  without dumping packet contents or credentials.
- Forced full-speed USB (default off) for topology/enumeration diagnostics.
  It limits the root bus to 12 Mbit/s and can avoid HS-hub transaction translation.

The serial summary prints every five seconds. The screen's Diagnostics tab
scrolls, and Logs shows the latest 4 KiB tail from a 16 KiB application-log ring.
Keep a UART capture for ROM/bootloader output, direct `printf` messages, power
resets or crashes that occur before the screen starts.

## Sources and provenance

Protocol request layouts, framing and register identities were checked against
primary upstream implementations and documentation:

- [Espressif ECM host documentation](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_host/usb_ecm.html)
  and [RNDIS host documentation](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_host/usb_rndis.html).
  Upstream hardware results for other ESP targets are not S31 results.
- [CherryUSB RTL8152 initializer](https://github.com/cherry-embedded/CherryUSB/blob/05007a0cdcf480ff5e870ce142bc92c217060fc8/class/vendor/net/usbh_rtl8152.c)
  and [AX88772 reset reference](https://github.com/cherry-embedded/CherryUSB/blob/05007a0cdcf480ff5e870ce142bc92c217060fc8/class/vendor/net/usbh_asix.c).
  The extracted RTL8152 code retains its Apache-2.0 copyright/license in
  `main/vendor/`. Port changes replace USB/OS glue with checked transport,
  cap reset polling, reject unsupported revisions, read the hardware MAC from
  PLA registers, and disable aggregation/VLAN stripping/checksum offload while
  enabling promiscuous reception. Host I/O failures propagate to setup failure.
- Linux upstream reference for protocol/register facts and OEM identities:
  [CDC NCM](https://github.com/torvalds/linux/blob/master/drivers/net/usb/cdc_ncm.c),
  [ASIX families](https://github.com/torvalds/linux/blob/master/drivers/net/usb/asix_devices.c),
  [AX88179/178A](https://github.com/torvalds/linux/blob/master/drivers/net/usb/ax88179_178a.c),
  [DM96xx](https://github.com/torvalds/linux/blob/master/drivers/net/usb/dm9601.c),
  [LAN95xx register definitions](https://github.com/torvalds/linux/blob/master/drivers/net/usb/smsc95xx.h).
- Your MIT-licensed MS2109 project's bounded log ring is reused for on-screen
  application-log retention. No USBView GPL ID database/application source is
  included.

Host tests cover protocol bounds, negotiation, register models, control failures,
configuration selection/fallback, manual selection, unplug/replug, EP0 timeout,
queue lifetime and malformed aggregates. A hardware campaign should cover at
least one adapter per native family and each advertised class mode, with cold
starts, DHCP/ARP/ping, bidirectional traffic and hotplug under load. Record VID:PID,
bcdDevice, selected configuration/driver, UART logs and observed failures; do
not turn a mock-model pass into a retail-adapter compatibility certification.
