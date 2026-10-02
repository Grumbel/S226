````markdown
# S226 BLE Reverse Engineering

Reverse engineering the Bluetooth Low Energy protocol of the **S226 fitness tracker/watch**, with the goal of communicating with it from Linux without the proprietary H-Band application.

## Heart-rate app (C++ / Qt)

```bash
nix run .#              # GUI: s226-hr
nix run .#s226-cli -- --list
nix run .#s226-cli      # terminal heart rate (-v for protocol log, --bp for blood pressure)
```

`s226-hr` shows the live heart rate as a big number that scales with the
window, with today's step count and cadence below it. The cadence is measured
between the moments the watch's step counter changes (polled every
second), so it follows the watch's own update rate (F11 / Esc for full screen, Ctrl+L for the log). A graph below shows
bpm and spm over the last 1 min to 2 h (pick the span in the toolbar;
hover for exact values). Every reading is also appended to a CSV file per
day, `~/.local/share/s226/s226-hr/s226-hr-YYYY-MM-DD.csv` (columns
`time,bpm,spm`), and the graph is refilled from it on restart. Tick
**Metronome** (or press M) to get a click on every beat; the heart icon
pulses along either way. It connects on start (`--no-connect` to skip),
reconnects when the watch drops out, and restarts the measurement each
time the watch ends one by itself (after ~30-50 s).

### Installing

The package installs the desktop integration under the app ID
`s226-hr` (the binary name, not a reverse-DNS ID): a `.desktop` launcher (with "Start in Full
Screen" and "Start Without Connecting" actions), an SVG icon plus PNG
sizes, AppStream metadata, the `s226-hr(1)` and `s226-cli(1)` man pages
and the udev rule. On NixOS, put the flake's package in
`environment.systemPackages` (launcher, icon, man pages) and
`services.udev.packages` (device access). With plain CMake:
`cmake -B build -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build && sudo cmake --install build`.

### Picking the Bluetooth dongle

The app drives a USB Bluetooth controller directly over raw HCI (like
Bumble), so BlueZ cannot interfere. Controllers are found by their USB
Bluetooth interface (class e0/01/01), not by name. That matters for
WiFi/BT combo chips such as `0bda:b82c`, which calls itself "802.11ac
NIC". Only the Bluetooth interface is taken over; WiFi on the same chip
keeps working.

`s226-cli --list` prints what is available. Wherever a controller can be
chosen (`--controller` in both tools, the drop-down in the GUI), any of
these work:

| Selector | Example |
|----------|---------|
| automatic (default) | `auto` |
| index from `--list` | `0` |
| vendor:product | `0bda:b82c` (`usb:0bda:b82c` also accepted) |
| USB port path | `1-10.1` |
| kernel adapter name | `hci0` |

While the app runs, the kernel's `btusb` driver is detached from that
dongle (the `hciN` adapter disappears) and it is re-attached on exit.
`bluetoothd` does not need to be stopped, but anything else using that
adapter loses it in the meantime.

**Permissions:** the user needs read/write access to
`/dev/bus/usb/BBB/DDD`. `udev/70-s226-bluetooth.rules` grants it to the
logged-in user for every USB Bluetooth controller. On NixOS, add the
package to `services.udev.packages` (it installs the rule to
`lib/udev/rules.d`).

**Realtek dongles** need the kernel to have loaded their patch firmware
once (it survives the takeover). If the log warns about "unpatched"
firmware, re-plug the dongle and try again.

### Code layout

| Path | What |
|------|------|
| `lib/` | `s226ble` library, plain C++20 + libusb, no Qt |
| `lib/include/s226/protocol.hpp` | S226 packet builders and decoders (pure functions) |
| `lib/include/s226/usb.hpp` | USB Bluetooth controller discovery and selectors |
| `lib/include/s226/watch.hpp` | `s226::Watch`: scan, connect, bind, heart rate / blood pressure |
| `lib/src/` | private: libusb HCI transport, minimal LE host (HCI, L2CAP, ATT) |
| `app/` | Qt 6 GUI (`s226-hr`) |
| `cli/` | `s226-cli` |

Development: `nix develop`, then `cmake -B build && cmake --build build && ctest --test-dir build`.

The Python tools (`s226.py`, `s226_bumble.py`) are still available as
`nix run .#s226` / `nix run .#s226-bumble`.

## Hardware

Observed device information:

- Device: **S226**
- Display: 1.3", 240×240
- MCU: **Nordic nRF52832**
- Bluetooth: BLE 4.0
- Firmware shown on watch: **31.06**
- H-Band identifies it as:

  ```text
  4a:cd 851 01.31.06
````

The companion application is **H-Band**.

## Linux Environment

Testing is being performed with:

* Linux
* BlueZ
* `bluetoothctl`
* `btmgmt`
* `btmon`
* Python
* [Bleak](https://bleak.readthedocs.io/)

The Bluetooth adapter is a Realtek USB adapter.

## BLE Discovery

The S226 does **not advertise continuously**. It appears to wake up and advertise for a short period, then disappear again.

This makes the following approach unreliable:

```text
scan for devices
        ↓
stop scan
        ↓
connect to discovered address
```

By the time the connection is attempted, the watch may have stopped advertising.

The Python program therefore uses a Bleak scanner callback and attempts to connect **immediately when the S226 advertisement is detected**.

The watch has also been observed disappearing between scans.

## BLE Address

The S226 currently appeared as:

```text
FD:32:EF:97:4A:CD
```

BlueZ reports it as:

```text
Device FD:32:EF:97:4A:CD (random)
```

Therefore this is a **random BLE address**, rather than a stable public address.

The watch itself displays:

```text
4A:CD
```

which corresponds to the final two bytes of the observed BLE address.

The H-Band app similarly identifies it as:

```text
4a:cd
```

### Manufacturer advertisement

The S226 advertises manufacturer data with ID:

```text
0xf8f8
```

Example:

```text
ManufacturerData.Key: 0xf8f8

ManufacturerData.Value:
  cd 4a 97 ef 32 fd
```

Interestingly, this is exactly the BLE address in reverse byte order:

```text
BLE address:
  FD:32:EF:97:4A:CD

Advertisement:
  CD 4A 97 EF 32 FD
```

This provides a useful way of identifying the device even if its advertised name isn't available.

## Advertisement

A raw `btmon` capture showed:

```text
LE Advertising Report
    Event type: Connectable undirected - ADV_IND
    Address type: Random
    Address: 29:6C:12:75:F9:A6
    Data length: 26

Advertising Data:
    02 01 02
    16 16 f1 fc
    04 72 ad f5 da 3a 29 18 7a
    6e 17 22 96 43 10 f6 5e e1 92
```

BlueZ decoded this as:

```text
Flags: 0x02
  LE General Discoverable Mode

Service Data:
  UUID: 0xfcf1
```

with 19 bytes of service data:

```text
04 72 ad f5 da 3a 29 18 7a
6e 17 22 96 43 10 f6 5e e1 92
```

The significance of this `0xfcf1` service data is not yet known.

Note that the address in this capture was different from the later:

```text
FD:32:EF:97:4A:CD
```

This is consistent with the watch using random/private BLE addresses.

## Connection

A connection has successfully been established from Linux using:

```text
bluetoothctl
```

The important successful state was:

```text
Device FD:32:EF:97:4A:CD (random)

Connected: yes
ServicesResolved: yes
```

The watch does not appear to require traditional Bluetooth pairing.

BlueZ reports:

```text
Paired: no
Bonded: no
Trusted: no
LegacyPairing: no
```

This is normal for a BLE device which uses application-level communication rather than normal Bluetooth pairing.

## GATT Services

After connecting, BlueZ discovered the following services.

### Generic Access

```text
00001800-0000-1000-8000-00805f9b34fb
```

Standard BLE Generic Access service.

Characteristics include:

```text
00002a00  Device Name
00002a01  Appearance
00002a04  Peripheral Preferred Connection Parameters
00002aa6  Central Address Resolution
```

### Generic Attribute

```text
00001801-0000-1000-8000-00805f9b34fb
```

Standard GATT service.

### Vendor service

```text
f0080001-0451-4000-b000-000000000000
```

Characteristics:

```text
f0080002-0451-4000-b000-000000000000
f0080003-0451-4000-b000-000000000000
```

These are likely candidates for the main communication protocol.

The exact properties and data format still need to be investigated.

### Vendor service

```text
f0020001-0451-4000-b000-000000000000
```

Characteristics:

```text
f0020002-0451-4000-b000-000000000000
f0020003-0451-4000-b000-000000000000
```

These are another strong candidate for application communication.

### Tencent service

```text
0000fee7-0000-1000-8000-00805f9b34fb
```

Bluetooth SIG assigned this UUID to:

```text
Tencent Holdings Limited
```

Characteristics:

```text
0000fea1-0000-1000-8000-00805f9b34fb
0000fea2-0000-1000-8000-00805f9b34fb
0000fec9-0000-1000-8000-00805f9b34fb
```

The exact use by the S226 is unknown.

BlueZ displays the first two as:

```text
Intrepid Control Systems, Inc.
```

and `fec9` as:

```text
Apple, Inc.
```

These company names are simply the Bluetooth SIG assignments for the UUIDs and do **not necessarily mean that Apple, Tencent, or Intrepid manufactured the watch or developed its firmware**.

### HID service

The watch exposes:

```text
00001812-0000-1000-8000-00805f9b34fb
```

Human Interface Device.

Characteristics include:

```text
00002a4e  Protocol Mode
00002a4d  Report
00002a4b  Report Map
00002a33  Boot Mouse Input Report
00002a4a  HID Information
00002a4c  HID Control Point
```

There are three HID Report characteristics:

```text
00002a4d
00002a4d
00002a4d
```

The reason for the HID service is currently unknown. It may be used for some watch interaction/control function rather than actual mouse functionality.

The Report Map needs to be examined.

## GATT Structure Observed

The complete structure discovered so far is approximately:

```text
1800 Generic Access
 ├─ 2a00 Device Name
 ├─ 2a01 Appearance
 ├─ 2a04 Peripheral Preferred Connection Parameters
 └─ 2aa6 Central Address Resolution

1801 Generic Attribute

f0080001 Vendor
 ├─ f0080002
 └─ f0080003

f0020001 Vendor
 ├─ f0020002
 └─ f0020003

fee7 Tencent
 ├─ fea1
 ├─ fea2
 └─ fec9

1812 Human Interface Device
 ├─ 2a4e Protocol Mode
 ├─ 2a4d Report
 ├─ 2a4d Report
 ├─ 2a4d Report
 ├─ 2a4b Report Map
 ├─ 2a33 Boot Mouse Input Report
 ├─ 2a4a HID Information
 └─ 2a4c HID Control Point
```

Many characteristics have CCCD descriptors:

```text
00002902-0000-1000-8000-00805f9b34fb
```

which means they can potentially be subscribed to for notifications/indications.

## Pairing / Authentication

The S226 is visible in the phone's normal Bluetooth "Available devices" list, but the H-Band application appears to communicate with it as a BLE peripheral.

Linux reports:

```text
Paired: no
Bonded: no
LegacyPairing: no
```

A successful GATT connection does not by itself imply Bluetooth pairing or bonding.

The important distinction is:

```text
BLE advertisement
       ↓
BLE connection
       ↓
GATT service discovery
       ↓
application protocol
```

Pairing/bonding is an optional security layer and may not be used at all.

It is possible that H-Band performs authentication **inside the vendor protocol** instead of using standard BLE pairing. This remains to be determined.

## Current Python Tool

`s226.py` uses Bleak to:

1. Scan for the S226.
2. Detect the device from its advertisement.
3. Recognize it by name or `0xf8f8` manufacturer data.
4. Immediately connect using the `BLEDevice` returned by the scanner.
5. Enumerate all GATT services / characteristics / descriptors.
6. Read every readable characteristic.
7. Subscribe to every notify/indicate characteristic.
8. Listen for packets for a configurable period.

The immediate-connect behavior is important because the watch appears to sleep shortly after advertising.

### Usage

```bash
# Default: scan 30s, connect 25s × 3 retries, listen 30s
nix run .#

# Long scan window (watch only advertises briefly after power-on / wake)
nix run .# -- --scan-timeout 300 --connect-timeout 30 --connect-retries 5

# Only log advertisements (no connect attempt)
nix run .# -- --scan-only --scan-timeout 120

# Longer listen window while interacting with the watch
nix run .# -- --listen 120

# Also write a full transcript
nix run .# -- --log-file s226-$(date +%Y%m%d-%H%M%S).log --listen 60

# Skip the (verbose) GATT dump when only notification traffic is needed
nix run .# -- --no-gatt-dump --listen 90
```

Available flags:

| Flag | Default | Description |
|------|---------|-------------|
| `--scan-timeout SECONDS` | 30 | How long to wait for an advertisement |
| `--connect-timeout SECONDS` | 40 | Timeout for each connection attempt |
| `--connect-retries N` | 3 | Retry connect this many times on failure |
| `--listen SECONDS` | 30 | How long to stay connected and print notifications |
| `--log-file PATH` | (none) | Mirror all console output to a file |
| `--no-gatt-dump` | false | Skip reading/printing the full GATT database |
| `--read-values` | false | Also read char/descriptor values (often disconnects) |
| `--scan-only` | false | Log advertisements only; do not connect |
| `--address ADDR` | (none) | Skip scan; connect directly to this address |

Known standard and vendor UUIDs are annotated with human-readable names in the output to make logs easier to scan.

### Connection reliability notes

The S226 advertises only for a short window. After the first power-on it is often easier to catch; afterwards it may sleep until the user interacts with the watch or the H-Band app wakes it.

Connection timeouts are common when the advertisement ends before BlueZ finishes the connection. Use a long `--scan-timeout`, a generous `--connect-timeout`, and several `--connect-retries`. Prefer connecting immediately on advertisement (what this tool does) rather than a separate scan-then-connect.

Observed failure modes:

* Device only appears at very short range (e.g. ~10 cm from the USB dongle). Treat this as a weak antenna / RF path issue on the adapter or watch; keep the watch against the dongle while testing.
* `TimeoutError` / connect timeout after a successful find — the watch is only connectable for a brief moment after each advertisement. The tool now **keeps scanning while connecting** so BlueZ does not drop the random-address device from its cache, and retries both the live `BLEDevice` object and a plain address-string connect.
* `failed to discover services, device disconnected` — GATT link comes up, then the watch drops it during service discovery. This is the dominant failure mode so far and matches Veepoo behaviour when the app-level password packet is not sent immediately. Retries now wait for a **fresh advertisement** each time instead of hammering a stale BlueZ device path.
* `Service Discovery has not been performed yet` — fixed by calling `get_services()` after `connect()`.

Useful parallel diagnostics while connecting:

```bash
sudo btmon | tee btmon-$(date +%Y%m%d-%H%M%S).log
```

Also try disabling USB autosuspend for the Realtek adapter if connects are flaky:

```bash
# find the device
lsusb
# then, for the corresponding sysfs path:
echo on | sudo tee /sys/bus/usb/devices/.../power/control
```

### Veepoo / H-Band protocol

The companion app is **H-Band** (Veepoo Technology). After a successful BLE GATT connection the official SDK **must** perform a password verification step (`confirmDevicePwd`) with the default password `"0000"`. Only after that does the device expose full functionality and stay usefully connected.

The binary command format for that password exchange lives inside the closed-source Veepoo `vpprotocol` library (command family reported as `0xF0` in secondary documentation). Until we capture real H-Band ↔ S226 HCI traffic or reverse the packet layout, Linux-side tools can only:

1. Obtain a stable GATT connection (this tool).
2. Dump services / characteristics / notifications.
3. Observe whether the watch disconnects quickly without the auth packet.

Capturing phone-side HCI logs while H-Band connects is the highest-value next experiment.

**Current status:** connect is intermittent (watch often ignores link
requests). When the ACL link does come up, a Veepoo **0xA1 bind packet**
must be written quickly or the watch drops the connection. A full phone
HCI capture of H-Band is documented in [PROTOCOL.md](PROTOCOL.md).

## Protocol summary

See **[PROTOCOL.md](PROTOCOL.md)** for the HCI-derived details. Short version:

| Direction | Characteristic | Role |
|-----------|----------------|------|
| watch → phone | `f0080002` | notifications |
| phone → watch | `f0080003` | Write Command commands |

After connect the tool enables notify on `f0080002` and sends a 20-byte
`0xA1` packet (time + profile bytes) to `f0080003`. Use `--no-auth` to
skip that for experiments.

## Device behaviour (short)

* Watch UI is local-only; it does not push to the phone on its own.
* Phone connection sets **time**, can toggle features (e.g. stopwatch menu),
  and can start a **30 s HR** session that streams to the phone.
* Details: [PROTOCOL.md](PROTOCOL.md).

## Remaining reverse-engineering targets

* Minimal 0xA1 payload; meaning of notification opcodes (`0xA7`, `0xAD`, …)
* Command set on `f0080003` (`d8`, `f4`, `aa`, `d1`/`d3`/`d4`, …)
* Role of services `f002` and `fee7` / HID
* Correlate notifications with UI actions on the watch
* Advertising / manufacturer data fields over time

## Goal

Standalone Linux tool path:

```text
scan → connect → 0xA1 bind → notifications → commands (steps, HR, …)
```

```
```


## Bumble backend (recommended on Linux)

BlueZ/`bluetoothd` injects SMP pairing when it sees the watch HID service,
which tears down the link. The Bumble tool drives the USB dongle over **raw
HCI** and never goes through bluetoothd.

```bash
# 1. Release the dongle from BlueZ
sudo systemctl stop bluetooth

# 2. Optional: allow userspace USB access (pick your dongle from lsusb)
# sudo chmod o+rw /dev/bus/usb/BBB/DDD

# 3. Run
nix run .#s226-bumble -- --transport usb:0
# or with explicit address:
nix run .#s226-bumble -- --transport usb:0 --address FD:32:EF:97:4A:CD
```

### Live heart rate

```bash
sudo nix run .#s226-bumble -- --transport usb:0 --hr
```

Binds, sends `d0 01`, and shows the BPM as it streams in (~1/s). The
watch ends a session after ~30-50 s; the tool restarts it automatically.
Ctrl-C sends `d0 00` and disconnects. `--hr-duration SEC` stops after a
fixed time. Blood-pressure results (`--probe bp`) are decoded too.

Flags: `--hr`, `--hr-duration SEC`, `--no-auth`, `--no-notify`, `--notify-after-a1`, `--listen SEC`,
`--scan-timeout SEC`, `-v`.

Re-enable BlueZ when done: `sudo systemctl start bluetooth`.
