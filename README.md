````markdown
# S226 BLE Reverse Engineering

Reverse engineering the Bluetooth Low Energy protocol of the **S226 fitness tracker/watch**, with the goal of communicating with it from Linux without the proprietary H-Band application.

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
5. Enumerate all GATT services.
6. Enumerate all characteristics.
7. Enumerate descriptors.
8. Read every readable characteristic.
9. Subscribe to every notify/indicate characteristic.
10. Listen for packets for a configurable period.

The immediate-connect behavior is important because the watch appears to sleep shortly after advertising.

## Current Reverse Engineering Targets

The next useful things to determine are:

### 1. Characteristic properties

Determine which of these are:

```text
read
write
write-without-response
notify
indicate
```

especially:

```text
f0080002
f0080003

f0020002
f0020003

fea1
fea2
fec9
```

### 2. Notification traffic

Subscribe to all notification characteristics and observe what the watch sends without any interaction.

Then repeat while performing actions such as:

* opening the watch menus
* changing screens
* starting/stopping activity tracking
* measuring heart rate
* changing the displayed mode
* triggering an alarm
* moving around
* changing the watch face

The goal is to correlate packets with actions.

### 3. H-Band traffic

The most useful next experiment would be capturing the actual BLE traffic between:

```text
H-Band ↔ S226
```

while performing known operations.

A Bluetooth HCI capture from the phone would allow comparison with the Linux-generated traffic.

### 4. Advertising data

The `0xfcf1` service data should be monitored over time.

Determine whether its 19-byte payload changes with:

* battery level
* steps
* heart rate
* time
* activity
* connection state

### 5. Authentication

Determine whether H-Band:

* uses BLE pairing/bonding
* writes an initialization packet
* performs a challenge/response
* sends a device identifier
* sends a fixed authentication token

before the watch starts sending useful data.

## Goal

The eventual goal is a standalone Linux implementation capable of communicating with the S226 without H-Band, ideally supporting:

```text
connect
    ↓
authenticate (if necessary)
    ↓
read device information
    ↓
read battery
    ↓
read activity/steps
    ↓
read heart-rate data
    ↓
read/write configuration
    ↓
possibly synchronize time
```

The protocol should first be understood from passive observations before attempting arbitrary writes to the watch.

```
```
