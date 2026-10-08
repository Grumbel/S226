# VeePoo / H+Band 2.0 BLE Protocol Reverse Engineering

**Source APK:** `H+Band+2.0_4.9.4_APKPure.apk`  
**Package:** `com.veepoo.hband`  
**Analysis date:** 2026-10-08

This document consolidates the reverse-engineered Bluetooth Low Energy (BLE) protocol used by the H+Band / VeePoo fitness watch family.

---

## 1. GATT Services & Characteristics

| Role | UUID | Notes |
|------|------|-------|
| **Primary Service** (internally “Battery”) | `F0080001-0451-4000-B000-000000000000` | Main command/response service |
| Notify / Read characteristic | `F0080002-0451-4000-B000-000000000000` | Device → phone notifications & responses |
| Write / Config characteristic | `F0080003-0451-4000-B000-000000000000` | Phone → device commands |
| **BP / ADC Service** | `F0020001-0451-4000-B000-000000000000` | Blood pressure / raw ADC / ECG-related |
| BP Notify | `F0020002-0451-4000-B000-000000000000` | |
| BP Config | `F0020003-0451-4000-B000-000000000000` | |
| Nordic OAD / DFU | `00001530-1212-EFDE-1523-785FEABCD123` | Classic Nordic DFU |
| Secure DFU (nRF) | `0000FE59-0000-1000-8000-00805F9B34FB` | Chars: `8EC90001-…` (control), `8EC90002-…` (packet) |
| Another OAD service | `A6ED0401-D344-460A-8075-B9E8EC90D71B` | (related chars `a6ed0402`…`a6ed0404`) |
| CCCD | `00002902-0000-1000-8000-00805f9b34fb` | Client Characteristic Configuration Descriptor |

**All normal application traffic goes through the F008… service.**  
Commands are written to the **Config** characteristic; the device replies via notifications on the **Read** characteristic.

---

## 2. Connection & Authentication Flow

1. Scan → connect → discover services.
2. Locate the F008… service (and optionally the BP service).
3. Enable notifications (write CCCD = `0x0100` on the Read characteristic).
4. Send password/time-sync packet (`0xA1`).
5. On success the device returns status + device number + OAD version.
6. App issues a series of “read” commands (battery, settings, sleep, sport, etc.).

### Default password
`0000`

### Password / time-sync packet (`0xA1`)

```
[0]   0xA1
[1]   pwd high byte
[2]   pwd low byte          (password treated as decimal int → 2 bytes)
[3]   type: 0 = check+set time, 1 = change password
[4-10] system time: YY_hi, YY_lo, MM, DD, HH, mm, ss
[11]  is_24h (1 = 24-hour mode)
[12]  0x01
```

### Auth reply (`0xA1` notification)

```
[0]   0xA1
[3]   status:
        0 = check fail
        1 = check success
        2 = set fail
        3 = set success
        4 = read fail
        5 = read success
        6 = check + time success
[4-5] device number (big-endian hex)
[6-9] firmware version (major.minor.patch[.build])
[11]  turn-wrist function (0=supported+off, 1=supported+on, 2=not supported)
[18]  find-phone function
[19]  check-wear function
```

---

## 3. Command Header Reference

| Header (signed) | Hex  | Purpose |
|-----------------|------|---------|
| -95 | `0xA1` | Password / time sync |
| -96 | `0xA0` | Read battery |
| -93 | `0xA3` | Person info |
| -92 | `0xA4` | Device number |
| -89 | `0xA7` | Device function / capabilities |
| -88 | `0xA8` | Current sport / steps |
| -85 | `0xAB` | Alarm (classic 3-alarm) |
| -84 | `0xAC` | Heart warning |
| -83 | `0xAD` | Device message function / notification support |
| -82 | `0xAE` | Find watch by phone |
| -81 | `0xAF` | Disconnect watch |
| -79 | `0xB1` | Screen light |
| -78 | `0xB2` | Count-down |
| -77 | `0xB3` | All settings |
| -76 | `0xB4` | Screen light time |
| -75 | `0xB5` | Find phone by watch |
| -74 | `0xB6` | Take photo |
| -72 | `0xB8` | Person watch setting |
| -71 | `0xB9` | Multi-alarm |
| -68 | `0xBC` | HID bind |
| -63 | `0xC1` | Phone / message alert control |
| -62 | `0xC2` | Send content (notification text) to watch |
| -57 | `0xC7` | Read screen style |
| -56 | `0xC8` | Weather |
| -52 | `0xCC` | Big-data content |
| -48 | `0xD0` | Current heart-rate read |
| -47 | `0xD1` | Original data (5-min steps/HR etc.) |
| -46 | `0xD2` | SpO₂ original |
| -45 | `0xD3` | Sport model CRC |
| -44 | `0xD4` | Sport model origin |
| -43 | `0xD5` | Sport model open/close |
| -41 | `0xD7` | Drink |
| -40 | `0xD8` | Current sport model |
| -39 | `0xD9` | HRV |
| -33 | `0xDF` | Original DF |
| -32 | `0xE0` | Sleep |
| -31 | `0xE1` | Long-seat (sedentary) |
| -30 | `0xE2` | Check wear |
| -17 | `0xEF` | Battery manager |
| -15 | `0xF1` | Device debug |
| -12 | `0xF4` | Change watch language |
| 1   | `0x01` | Battery auto-callback / music / PTT |
| 3   | `0x03` | Battery low-power |
| 80  | `0x50` | Big-data transfer |
| 112 | `0x70` | GPS data |
| 113 | `0x71` | Bluetooth 3.0 |
| 114 | `0x72` | Contact |
| -128| `0x80` | SpO₂ continuous read start/stop |
| -127| `0x81` | Fatigue (FTG) read |
| -126| `0x82` | Breath rate read |
| -123| `0x85` | Women / menses setting |
| -122| `0x86` | SpO₂ breath-break remind |
| -112| `0x90` | BP |
| -111| `0x91` | BP model |
| -109| `0x93` | ECG data app |
| -106| `0x96` | ECG ID get |
| -105| `0x97` | ECG ID use |
| -103| `0x99` | Music |
| -94 | `0xA2` | OAD command |

---

## 4. Phone → Watch Notifications (show alerts on watch)

### 4.1 Alert / control commands (`0xC1`)

Written to Config characteristic.

| Purpose | Bytes |
|---------|-------|
| SMS / message alert | `C1 01 01 01` |
| Incoming-call alert | `C1 01 06 00` |
| Stop vibration / clear call | `C1 00 00 00` |

**Call flow**
1. Phone rings → write `PHONE_CMD` (`C1 01 06 00`)
2. Send caller name/number as `0xC2` content packet(s)
3. Call answered or hung up → write `MSG_PHONE_CLOSE_CMD` (`C1 00 00 00`)

### 4.2 Text content packets (`0xC2`)

Every content packet is **exactly 20 bytes**:

```
Offset  Size  Meaning
------  ----  -------------------------------------------------
  0       1   0xC2
  1       1   App / message type (see table)
  2       1   Payload length in this packet (1–14)
  3       1   Total number of packets in this message
  4       1   Current packet index (1-based)
  5       1   Flags / subtype
                0 = phone number (no contact name)
                1 = contact name exists (or title packet)
                2 = message body / social content
  6–19   14   UTF-8 payload (zero-padded if shorter)
```

#### Message type IDs (byte 1)

| ID | App / source |
|----|--------------|
| 0  | Phone call / SMS (caller name or number) |
| 2  | WeChat |
| 3  | QQ |
| 4  | Sina Weibo |
| 5  | Facebook |
| 6  | Twitter |
| 7  | Flickr |
| 8  | LinkedIn |
| 9  | WhatsApp |
| 10 | LINE |
| 11 | Instagram |
| 12 | Snapchat |
| 13 | Skype |
| 14 | Gmail |
| 15 | DingTalk |
| 16 | WeChat Work |
| 17 | Other / generic apps |

#### Multi-packet rules
- UTF-8 text is split into **14-byte chunks**.
- Packet index starts at **1**.
- Last packet has a smaller length field (byte 2).
- Packets are typically sent with ~120 ms delay between them.
- For SMS/phone that have title (contact name) + body, title packets are sent first, then body packets; the total-packet count covers both.

#### Example – single-packet call content (“Alice”)

```
C2 00 05 01 01 01 41 6C 69 63 65 00 00 00 00 00 00 00 00 00
```

#### Example – WeChat message “Hello world!”

```
C2 02 0C 01 01 02 48 65 6C 6C 6F 20 77 6F 72 6C 64 21 00 00
```

### 4.3 Prerequisites
The watch must report support for the notification type (via `0xAD` capability packet after password handshake). The phone only sends a given type if the device reported support **and** the user enabled it in app settings.

---

## 5. Notification Formats (Watch → Phone)

All normal notifications arrive on the Read characteristic.  
Packets are almost always **exactly 20 bytes**.  
**Byte 0** = command header (same values used in the write direction).

### 5.1 Battery (`0xA0`)

```
[0] 0xA0
[1] model?
[2] ?
[3] state?
[4] level (0–4)
```

### 5.2 Current steps / sport (`0xA8`)

Normal current steps (4-byte **big-endian**):

```
[1..4] steps  (FFFFFFFF → 0)
```

Sport-model variant uses different endianness + extra fields:

```
[2..5]  steps
[6..9]  distance
[10..13] calories
```

### 5.3 Sleep (`0xE0`) – multi-packet

```
[0]   0xE0
[1]   packet index / end flag (0 = end of day)
[2]   ?
[3]   day index (0=today, 1=yesterday…)
[4..19] 16 bytes of payload (hex-concatenated into a continuous stream)
```

App collects packets until `[1] == 0`, then splits the concatenated payload into fixed-size sleep segments.

### 5.4 5-minute / Original data (`0xD1` / `0xDF`) – multi-packet + TLV

**Transport layer (each 20-byte ATT packet):**

```
[0]     header (0xD1 / 0xDF …)
[1]     current packet number
[2]     total packets
[3..4]  related to packet numbering / end detection
[5]     day index (or bit-field in drink-protocol variants)
[4..19] 16-byte payload chunks that are concatenated
```

End-of-day: roughly when `current == total` (bytes 1==3 and 2==4).

**After reassembly the content is a sequence of TLV blocks** (little-endian):

| Tag (signed) | Hex  | Content |
|--------------|------|---------|
| -79 | `0xB1` | Date/time block → TimeBean (Y,M,D,H) |
| -78 | `0xB2` | Step block → steps, distance, cal… |
| -77 | `0xB3` | Sleep block |
| -76 | `0xB4` | Rate / HR block |
| -75 | `0xB5` | Heart block |
| -74 | `0xB6` | Breath block |
| -73 | `0xB7` | HRV block |
| -72 | `0xB8` | BP block |
| -71 | `0xB9` | SpO₂ block |
| -70 | `0xBA` | Sleep-sport block |
| -69 | `0xBB` | Sleep-state block |
| -68 | `0xBC` | Reboot block |

Each TLV: `tag (1) | length (1) | data (length bytes)`.  
Some variants carry a CRC-16 over the block content.

### 5.5 Continuous SpO₂ (`0x80`)

**Start:** `{0x80, 0x01}`  
**Stop:**  `{0x80, 0x02}`

**Notification payload:**

```
[0] 0x80
[1] spState
[2] watchState
[3] SpO₂ value
[4] checking flag (1 or 2 = in progress)
[5] checking progress
[7] wave state (when length == 20)
[8] value-is-valid flag
```

### 5.6 Continuous breath rate (`0x82`)

**Start:** `{0x82, 0x01}`  
**Stop:**  `{0x82, 0x02}`

```
[0] 0x82
[1] spState
[2] watchState
[3] progress
[4] value
```

### 5.7 Fatigue (FTG) (`0x81`)

**Start:** `{0x81, 0x01}`  
**Stop:**  `{0x81, 0x02}`  
Similar continuous measurement layout to breath/SpO₂.

### 5.8 Continuous / auto-callback (`0x01`)

```
[0] 0x01
[1] subtype
      1 = music control
      2 = PTT / push-to-talk state
[2] further subtype (for music must be 1)
[3] action (1=next, 2=play/pause, …)
```

### 5.9 Device function / capability bitmap (`0xA7`)

```
[0]  0xA7
[19] sub-type (2 or 3)
```

When `[19]==2` the remaining bytes form a large capability map (protocol type, history days, sport-model support, screen-style count, breath/SpO₂/HRV/ECG/contact/HID/countdown flags, etc.).

### 5.10 Person-info reply (`0xA3`)

```
[0] 0xA3
[1] 1 = success, 0 = fail
```

### 5.11 BP / ADC / ECG path

Commands written on the main service; continuous/raw samples often arrive as notifications on the **BP service** (`F0020002-…`).  
Pairs of bytes are converted with swapped endianness into 16-bit ADC values.

### 5.12 Big-data / UI transfer (`0xCC` / `0x50`)

```
[0] header
[1] status (1=OK, 2=CRC fail, 3=flash CRC fail, 4=timeout)
[2..5] block id + CRC (little-endian 16-bit)
```

Used for watch-face / UI binary transfer with block acknowledgements and retransmission.

---

## 6. Selected Command / Setting Packets (Phone → Watch)

### 6.1 Classic 3-alarm (`0xAB`)

```
[0]   0xAB
[1-2] alarm0 hour, minute
[3]   alarm0 open (0/1)
[4-5] alarm1 hour, minute
[6]   alarm1 open
[7-8] alarm2 hour, minute
[9]   alarm2 open
[10]  0x01
```

Read: `{0xAB, 0,0,0,0,0,0,0,0,0, 6}`

### 6.2 Multi-alarm (`0xB9`)

```
[0] 0xB9
[1] operate: 0=clear, 1=set, 2=read
[2] alarm id
[3] hour
[4] minute
[5] open (0/1)
[6] repeat bitmask
[7] scene
[8-11] optional date (for non-repeating)
[18-19] CRC (on read)
```

### 6.3 Long-seat / sedentary (`0xE1`)

```
[0] 0xE1
[1] start hour
[2] start minute
[3] end hour
[4] end minute
[5] duration (minutes)
[6] action: 0=close, 1=open, 2=read
```

### 6.4 Night turn-wrist (`0xAA`)

```
OPEN:  {0xAA, 0x01}
CLOSE: {0xAA, 0x00}
READ:  {0xAA, 0x02, 0,0,1,1}
```

### 6.5 Check-wear (`0xE2`)

```
OPEN:  {0xE2, 0x01}
CLOSE: {0xE2, 0x00}
READ:  {0xE2, 0x02}
```

### 6.6 Weather (`0xC8`)

```
Read status:  {0xC8, 0x02, ...}
Set status:   {0xC8, 0x03, isOpen, weatherType_lo, ...}
```

### 6.7 Person info (`0xA3`)

```
[0] 0xA3
[1] height
[2] weight
[3] age
[4] sex (0=female, 1=male)
[5-6] target steps (uint16 BE)
[7-8] target sleep minutes (uint16 BE)
```

### 6.8 Language (`0xF4`)

```
Chinese: {0xF4, 0,0,0, 0}
English: {0xF4, 0,0,0, 1}
```

### 6.9 Find watch / take photo / disconnect

```
Find watch open:  {0xAE, 0x01}
Find watch close: {0xAE, 0x00}
Take photo open:  {0xB6, 0x01}
Take photo close: {0xB6, 0x00}
Disconnect:       {0xAF, 0x00}
```

### 6.10 Continuous HR (`0xD0`)

```
Start: {0xD0, 0x01}
Stop:  {0xD0, 0x00}
```

### 6.11 Enter OAD / DFU mode

```
{0xA2, 0x00, 0x00, 0x01}
```

---

## 7. Endianness & Helper Notes

- Many 16-bit values are stored little-endian; the helper `twoByteToInt(high, low)` is often called as `twoByteToInt(value[n+1], value[n])`.
- Some 32-bit step/calorie values are big-endian hex concatenation of consecutive bytes.
- Password and many single-byte fields use the low byte of an `intToBytes()` conversion.
- CRC-16 appears on larger blocks (UI transfer, multi-alarm read, DF blocks) via `AlarmCrcUtil.getAlarmCrc16`.

---

## 8. Key Classes in the APK (for further study)

| Class | Role |
|-------|------|
| `com.veepoo.hband.ble.BleProfile` | All UUIDs, headers, fixed command arrays |
| `com.veepoo.hband.ble.BleProfileUtil` | Header → action string mapping |
| `com.veepoo.hband.ble.BluetoothService` | GATT connection, write/notify, dispatch |
| `com.veepoo.hband.ble.readmanager.*` | Per-feature parsers (Sleep, Original, SP, Breath, Alarm, …) |
| `com.veepoo.hband.phone.MessageNotifiCollectService` | Android NotificationListener → `0xC2` packets |
| `com.veepoo.hband.phone.PhoneStatReceiver` | Call/SMS → `0xC1` + `0xC2` |
| `com.veepoo.hband.util.ConvertHelper` | Byte/hex/endian helpers, nickname truncation |

---

## 9. Minimal Sequence to Show a Notification on the Watch

1. (Optional) Write alert command:
   - SMS → `C1 01 01 01`
   - Incoming call → `C1 01 06 00`
2. Write one or more `0xC2` content packets (correct type ID + UTF-8 text).
3. For a call that ends → write `C1 00 00 00` to stop vibration.

All writes go to the **Config characteristic** of the main F008 service.

---

*End of document.*

---

## 10. Deep Dive – Additional Features

### 10.1 Women / Menses setting (`0x85`)

**Write packet (20 bytes):**

```
[0]   0x85
[1]   status / operate:
        0 = off / clear?
        1 = menstruation tracking
        2 = (status 2)
        3 = pregnancy mode
        4 = (status 4)
        5 = read
[2-5] last menses / expected date (YY_hi, YY_lo, MM, DD)  — omitted for status 0
[6]   cycle interval (days)
[7]   menses length (days)   (also copied to [13] in some paths)
[8-11] baby birthday (for pregnancy-related modes)
[12]  baby gender (0=?, 1=male, 2=female)
```

Date encoding: year as two hex-derived bytes, month/day as single bytes.

**Read:** `{0x85, 0x05, ...}`

---

### 10.2 Drink data (`0xD7`)

**Read command:** `{0xD7}` (or with trailing zeros)

**Multi-packet reply.** Each 20-byte packet contains packed timestamps:

```
[0]   0xD7
[1]   packet / type index
[2]   current packet #
[3]   total / end marker  (both 0xFF or both 0 → end)
[3-4] packed date (year 12 bits + month 4 bits)
[5-6] packed time (day 5 bits + hour 5 bits + minute 6 bits)
[7..] further drink volume / duration fields
```

End condition: `[2]==[3]==0xFF` or `==0`, or `[1]==0`.

---

### 10.3 Count-down timer (`0xB2`)

**Write:**

```
[0] 0xB2
[1] operate: set / read / cancel
[2] count-down ID
[3-5] seconds (24-bit, little-endian style via intToBytes)
[6] UI mode: 0=off, 1=open watch UI, 2=not test-by-watch
```

**Reply status in [1]:**
- 1 = setting
- 2 = read
- 3 = counting (in progress)
- 4 = ended
- `[2]==0` → operate failed

---

### 10.4 Screen brightness schedule (`0xB1`)

```
[0] 0xB1
[1] operate (1=set, 2=read)
[2] start hour
[3] start minute
[4] end hour
[5] end minute
[6] level (brightness)
[7] other level
[8] operate flag
```

Auto-brightness is inferred when level == otherLevel under certain conditions.

### 10.5 Screen-on duration (`0xB4`)

```
[0] 0xB4
[1] operate (1=set, 2=read)
[2] duration (seconds) when setting
```

**Reply carries:**
```
[3] current duration
[4] recommend duration
[5] max duration
[6] min duration
```

---

### 10.6 SpO₂ breath-break remind (`0x86`)

```
[0]  0x86
[1]  operate (1=set, 2=read)
[2]  start hour
[3]  start minute
[4]  end hour
[5]  end minute
[6]  remind time default
[7]  remind time
[8]  min oxygen threshold
[9]  (extra default)
[10] open status
```

**Status reply:** `[1]` success/fail, `[2]` operate type (1=set, 2=read).

---

### 10.7 All-settings / night SpO₂ monitor (`0xB3`)

Currently known type: `SPO2H_NIGHT_MONITOR`.

```
[0] 0xB3
[1] type id
[2] operate
[3] start hour
[4] start minute
[5] end hour
[6] end minute
… (further parameters)
```

---

### 10.8 Continuous Fatigue (FTG) (`0x81`)

**Start:** `{0x81, 0x01}`  
**Stop:**  `{0x81, 0x02}`

Notification layout analogous to breath/SpO₂:

```
[0] 0x81
[1] state
[2] watch state
[3] progress / value fields
…
```

Watch-state enum shared with SpO₂/breath:
`FREE, DETECT_BP, DETECT_HEART, DETECT_AUTO_FIVE, DETECT_SP, DETECT_FTG, UNKNOWN`

---

### 10.9 Sport-model mode (`0xD3` / `0xD4` / `0xD5`)

| Header | Purpose |
|--------|---------|
| `0xD5` | Open / close sport model |
| `0xD4` | Origin / data request |
| `0xD3` | CRC verification of sport-model data |

When a CRC packet arrives with `[1]==2` the app replies with a CRC-ack style packet after a short delay.

Sport-model live data (steps/distance/calories) uses the endianness described in §5.2.

---

### 10.10 ECG (`0x93` / `0x96` / `0x97`)

**App-side control (written to main Config char):**

```
READ_ECG_NOTIFY   = {0x93, 1, 1, 1, 0…0}   // enable ECG stream
READ_ECG_UNNOTIFY = {0x93, 1, 1, 0, 0…0}   // disable
READ_FTT_NOTIFY   = {0x93, 1, 3, 1, 0…0}   // fatigue-related stream
READ_FTT_UNNOTIFY = {0x93, 1, 3, 0, 0…0}
```

**ID management:**
- Get ID: header `0x96`
- Use ID: header `0x97`

Raw ECG / ADC samples typically arrive on the **BP service notify characteristic** (`F0020002-…`).  
16-bit samples are recovered by swapping byte pairs (`byte2HexForAdcTranslateIntArr`).

---

### 10.11 Contacts (`0x72`)

Contacts are transferred as multi-packet sequences on header `0x72`.  
Each contact entry is packed into one or more 20-byte frames containing:

- name (UTF-8, length-limited, via `ConvertHelper.getNickByte`)
- phone number
- index / total counts

The app side uses `ContactHandler` / `ContactParse` to assemble and sort entries.  
Exact per-byte layout is device-firmware dependent; the transfer follows the same 14-byte payload + sequence pattern used by `0xC2` content packets.

---

### 10.12 GPS data (`0x70`)

GPS activity is reported under header `0x70`.  
The `GpsDataOprate` handler reassembles multi-packet streams that contain:

- GPS state (`GPS_STATE`)
- Sport state (`GPS_SPORT_STATE`)
- Data-head state (`GPS_DATA_HEAD_STATE`)
- Longitude / latitude / distance
- CRC-protected blocks of track points

Packets are collected, CRC-checked, and converted into `GpsInfo` / `TGpsPoint` / `TGpsTravel` objects.  
Detailed point encoding follows a compact binary format (coordinates + timestamps) that is then expanded by `GpsConvertUtil`.

---

### 10.13 Big-data / UI transfer (`0x50` + `0xCC`)

**Base-info / enter UI mode (`0x50`):**

```
Read:  {0x50, 0x02, useTypeUI=1}
Set:   {0x50, 0x01, useTypeUI,
        addr0,addr1,addr2,addr3,   // 32-bit little-endian receive address
        len0,len1,len2,len3,       // 32-bit file length
        type0,type1,               // bin data type
        crc0,crc1,                 // image CRC id
        0x01}
```

**Block content (`0xCC`):**
- Phone sends UI/binary blocks.
- Watch acknowledges with status in byte `[1]`:
  - 1 = OK → advance to next block
  - 2 = CRC of received data wrong → retransmit
  - 3 = flash CRC wrong → retransmit
  - 4 = timeout → retransmit
- Block id + CRC are little-endian 16-bit values in the reply.

Transfer proceeds block-by-block until `current_block >= all_block`.

---

### 10.14 SpO₂ original history (`0xD2`)

Same multi-packet transport pattern as original 5-minute data (`0xD1`):

```
Request today:     {0xD2, 1, 0, 0}
Request yesterday: {0xD2, 1, 0, 1}
Request day-2:     {0xD2, 1, 0, 2}
```

Payload reassembly and end-of-day detection mirror `OriginalHander`.

---

### 10.15 Heart-warning (`0xAC`)

```
Read: {0xAC, 0, 0, 2}
```

Further threshold bytes are present in full setting packets (high/low HR limits).

---

### 10.16 Music control (watch → phone via `0x01`)

Already covered in §5.8. Watch can send next / play-pause / previous key events; the phone injects corresponding media-key events.

---

## 11. Shared Continuous-Measurement State Machine

SpO₂ (`0x80`), Breath (`0x82`), FTG (`0x81`) and related measurements share a common watch-state model:

| Value | Meaning |
|-------|---------|
| FREE | Idle |
| DETECT_BP | Blood-pressure measurement in progress |
| DETECT_HEART | Heart-rate measurement |
| DETECT_AUTO_FIVE | Automatic 5-minute detection |
| DETECT_SP | SpO₂ detection |
| DETECT_FTG | Fatigue detection |

A measurement notification that carries `watchState != FREE` for another sensor usually indicates the watch is busy and the new request may be rejected or queued.

---

## 12. Implementation Notes for a Client

1. **Always enable CCCD** on `F0080002-…` before any commands.
2. **Password first** – most devices ignore other commands until a successful `0xA1` exchange.
3. **Respect 20-byte MTU** – every application packet is padded/truncated to 20 bytes.
4. **Inter-packet delay** – ~120 ms between consecutive writes is used by the official app; shorter delays can cause lost packets on some firmwares.
5. **Capability check** – read the `0xA7` / `0xAD` replies after auth to know which features (and which notification types) the particular watch supports.
6. **Endianness** – treat multi-byte fields on a case-by-case basis; the helpers in `ConvertHelper` are the authoritative reference.

---

*Document extended with deep-dive sections for women/menses, drink, countdown, screen, SpO₂ remind, all-settings, FTG, sport-model, ECG, contacts, GPS, big-data/UI, SpO₂ history, heart-warning and the shared continuous-measurement state machine.*

---

## 13. Notification switch table (enable Call / SMS / apps on the watch)

**Header:** `0xAD` (`HEAD_DEVICE_MSG_FUNCTION`)  
**Direction:** Phone → Watch (write to Config characteristic)  
**Length:** 20 bytes

This is the “ad switch table” that turns notification types on or off on the device.  
Until the relevant bits are set to **ON**, the watch will ignore `0xC1` alerts and `0xC2` content for that type.

### Packet layout

```
Offset  Field
------  ----------------------------------------------------------
  0     0xAD
  1     0x01                    (set / write)
  2     Phone call              1 = ON,  2 = OFF
  3     SMS                     1 = ON,  2 = OFF
  4     WeChat                  0 = unsupported, 1 = ON, 2 = OFF
  5     QQ                      0 / 1 / 2
  6     Sina Weibo              0 / 1 / 2
  7     Facebook                0 / 1 / 2
  8     Twitter                 0 / 1 / 2
  9     Flickr                  0 / 1 / 2
 10     LinkedIn                0 / 1 / 2
 11     WhatsApp                0 / 1 / 2
 12     LINE                    0 / 1 / 2
 13     Instagram               0 / 1 / 2
 14     Snapchat                0 / 1 / 2
 15     Skype                   0 / 1 / 2
 16     Gmail                   0 / 1 / 2
 17     DingTalk                0 / 1 / 2
 18     WeChat Work             0 / 1 / 2
 19     Other                   0 / 1 / 2
```

### Value encoding

| Value | Phone / SMS        | Social / other apps              |
|-------|--------------------|----------------------------------|
| `0`   | (treated as off)   | **Not supported** by this watch  |
| `1`   | **ON**             | Supported **and ON**             |
| `2`   | **OFF**            | Supported **and OFF**            |

Phone call and SMS are only ever written as `1` or `2` by the official app.

### Practical examples

**Enable Call + SMS only (everything else off/unsupported):**
```
AD 01 01 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

**Enable Call + SMS, force all other *supported* types OFF:**
```
AD 01 01 01 02 02 02 02 02 02 02 02 02 02 02 02 02 02 02 02
```
(Use `00` instead of `02` for types the watch reported as unsupported.)

**All open (official helper pattern):**
```
AD 01 01 01 01 01 01 01 01 01 01 01 01 01 01 01 01 01 01 01
```
(then zero out slots that are not supported by the device)

### Reply from the watch (notification on Read characteristic)

Same header `0xAD`. Layout mirrors the write table:

- `[2]` phone call → 1 = open, 2 = closed  
- `[3]` SMS → 1 = open, 2 = closed  
- `[4]…[18]` apps → 0 = unsupported, 1 = open, 2 = closed  
- `[19]` other (low nibble; read path also checks high nibble == 0)

The device also pushes this table after a successful password handshake so the app can populate its toggles.

### Source in the APK

- Write builder: `MessageSettingActivity.getMsgSettingCmd()` / `getAllOpenCmd()` / `getAllCloseCmd()`
- Helpers: `getPhoneMsgCmd(checked)` → 1 or 2; `getSupportCmd(supported, open)` → 0 / 1 / 2
- Reply parser: `DeviceFucitonHandler.getDeviceMsgSupportData()` + `savePhoneMsgInfo` / `saveMsgInfo`


---

## 14. Person watch setting (`0xB8`) — full layout

**Header:** `0xB8` (`HEAD_PERSON_WATCH_SETTING`)  
**Characteristic:** Config `F0080003-0451-4000-B000-000000000000`  
**Length:** 20 bytes  

Two packages, selected by **byte[19]**:

| byte[19] | Package | Contents |
|----------|---------|----------|
| `0` (or not 1) | Package 1 | Main feature toggles |
| `1` | Package 2 | Long-click lock + message screen light |

### Common header

| Offset | Write | Reply |
|--------|-------|-------|
| 0 | `0xB8` | `0xB8` |
| 1 | `0x01` = set, `0x02` = read | Operate / status (`1` or `2`) |

**Read request (package 1):**
```
B8 02 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```
(`BleProfile.READ_B8`)

### Flag encoding (most fields)

| Value | Meaning |
|-------|---------|
| `0` | Feature not supported / N/A |
| `1` | Supported and **ON** |
| `2` | Supported and **OFF** |

Exceptions called out below (units, hour format).

### Package 1 (`byte[19] ≠ 1`)

| Offset | Flag | Notes |
|--------|------|--------|
| 2 | Units (km / inch) | `0` = unsupported; **`1` = metric (km)**; **`2` = imperial (inch)** |
| 3 | Hour format | **`1` = 24-hour**; **`2` = 12-hour** (when supported) |
| 4 | 5-minute auto heart rate | |
| 5 | 5-minute auto BP | |
| 6 | Sport over-goal remind | |
| 7 | Voice BP / heart | |
| 8 | Find-phone UI | |
| 9 | Stopwatch | |
| 10 | SpO₂ low remain | 0 / 1 / 2 |
| 11 | Wear-detect skin | 0 = unsupported; 1 = ON; 2 = OFF |
| 12 | Auto HRV | |
| 13 | Auto in-calling | |
| 14 | Disconnect remind | |
| 15 | *(unused in official write builder)* | |
| 16 | PPG | |
| 17 | *(unused in official write builder)* | |
| 18 | Music control | |
| 19 | `0` | Package 1 marker |

The official app always sends the **full** current toggle table, then overrides the single flag the user changed (`whichBeingSelect`).

**Reply path:** if `value[1] == 2` and `value[19] == 0`, the app may run `judageTheSame()` (sync km / hour / skin with phone) and write a corrected packet back. Otherwise flags are stored via `DeviceFucitonHandler.getDevicePersonWatchSetting()`.

### Package 2 (`byte[19] == 1`)

| Offset | Flag | Encoding |
|--------|------|----------|
| 0 | `0xB8` | |
| 1 | `0x01` set | |
| 2 | Long-click lock | 0 = N/A, 1 = ON, 2 = OFF |
| 3 | **Message screen light** | 0 = N/A, **1 = ON** (notification wakes screen), **2 = OFF** |
| 4–18 | `0` | |
| 19 | **`0x01`** | Package 2 marker |

**Enable “notification wakes screen”:**
```
B8 01 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01
```

**Disable:**
```
B8 01 00 02 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01
```

Reply for package 2 only parses `bArr[2]` (long-click lock) and `bArr[3]` (message screen light).

**Source:** `PersonWatchSettingActivity.getPWSettingCmd()` / `getPWSettingCmd2()`, `DeviceFucitonHandler.getDevicePersonWatchSetting()` / `handleB8Package2()`.

---

## 15. Message / notification on-screen duration

**There is no dedicated “how long a notification stays on screen” setting** in this protocol or APK.

| Mechanism | Role |
|-----------|------|
| `0xB8` package 2, byte[3] | Only whether a message **wakes** the screen (`IS_OPEN_MESSAGE_LIGHTSCREEN`) |
| `0xB4` screen light time | **Global** screen-on duration in **seconds** after any wake (button, raise-to-wake, or notification). See §10.5 |
| `0xC2` content packets | **No duration / timeout / display-time field** |

### `0xC2` layout (confirmed — no display duration)

```
[0]  0xC2
[1]  App type ID
[2]  Payload length this packet (1–14)
[3]  Total packets
[4]  Packet index (1-based)
[5]  Flags (0=number, 1=title/name, 2=body)
[6–19] UTF-8 payload
```

How long the alert remains visible is entirely firmware-side, gated by the global screen timeout (`0xB4`) and whether message screen-light is enabled (`0xB8` pkg2).


---

## 16. Brightness schedule (`0xB1`) — full layout

**Header:** `0xB1` (`HEAD_SCREEN_LIGTH`)  
**Length:** 20 bytes

### Write

| Offset | Meaning |
|--------|---------|
| 0 | `0xB1` |
| 1 | `0x01` = set, `0x02` = read |
| 2 | Start hour |
| 3 | Start minute |
| 4 | End hour |
| 5 | End minute |
| 6 | Level (brightness inside schedule window) |
| 7 | Other level (brightness outside window) |
| 8 | Operate mode |

**Operate mode (byte 8):**
- `1` = auto brightness
- `2` = manual
- App also treats as auto when: start=22:00, end=08:00, level=2, otherLevel=4

**Read request:** `B1 02 00…00`

### Reply

| Offset | Meaning |
|--------|---------|
| 1 | Success `1` / fail `0` |
| 2 | `1` = set reply, `2` = read reply |
| 3 | Start hour |
| 4 | Start minute |
| 5 | End hour |
| 6 | End minute |
| 7 | Level |
| 8 | Other level |
| 9 | Operate (when length > 9) |
| 10 | Max level supported by device |

**Source:** `ScreenLightHanlder.getScreenLightCmd()` / `getScreenData()` / `isAutoScreen()`.

---

## 17. Classic 3-alarm (`0xAB`) — full layout

**Header:** `0xAB` (`HEAD_ALARM`)

### Write (set all three alarms at once)

| Offset | Meaning |
|--------|---------|
| 0 | `0xAB` |
| 1 | Alarm 0 hour |
| 2 | Alarm 0 minute |
| 3 | Alarm 0 open (`0` / `1`) |
| 4 | Alarm 1 hour |
| 5 | Alarm 1 minute |
| 6 | Alarm 1 open |
| 7 | Alarm 2 hour |
| 8 | Alarm 2 minute |
| 9 | Alarm 2 open |
| 10 | `0x01` (set marker) |

**Read request:**
```
AB 00 00 00 00 00 00 00 00 00 06
```
(`byte[10] = 6` = read; `BleProfile.ALARM_READ`)

### Reply

| Offset | Meaning |
|--------|---------|
| 1 | Success `1` / fail `0` |
| 2 | Alarm 0 hour |
| 3 | Alarm 0 minute |
| 4 | Alarm 0 open |
| 5 | Alarm 1 hour |
| 6 | Alarm 1 minute |
| 7 | Alarm 1 open |
| 8 | Alarm 2 hour |
| 9 | Alarm 2 minute |
| 10 | Alarm 2 open |
| 11 | `1` = set result, `6` = read result |

Note: write and read-reply layouts differ (reply inserts status at [1] and shifts the three alarm triples).

**Source:** `AlarmHandler.getAlarmCmd()` / `getReturnData()` / `saveAlarm()`.

---

## 18. Multi-alarm (`0xB9`) — full layout

**Header:** `0xB9` (`HEAD_ALARM_MULTI`)  
Used when the device supports more than the classic 3 fixed alarms (UI max **20** alarms).

### Write

| Offset | Meaning |
|--------|---------|
| 0 | `0xB9` |
| 1 | Operate: `0` = clear, `1` = set, `2` = read |
| 2 | Alarm ID |
| 3 | Hour (0–23) |
| 4 | Minute |
| 5 | Open (`0` / `1`) |
| 6 | **Repeat bitmask** (7-bit weekday field; see below) |
| 7 | **Scene index** (grid position; see below) |
| 8–11 | Date if one-shot (`repeat == 0`) and not `0000-00-00` |
| 18–19 | CRC-16 (on read request; computed over stored alarms) |

**Read request:** operate = `2`, CRC in [18–19].

### Repeat bitmask (`byte[6]`) — detailed

The app stores a **7-character binary string**, then converts:

```text
Integer.parseInt(repeatStatus, 2)  →  0 … 127
```

`getRepeartStatusStr(int)` reverses that (`Integer.toBinaryString`, left-padded to 7 chars). Values **> 127** are forced to `"0000000"`.

#### Bit layout (on the wire / in the integer)

```text
bit  6        5        4        3        2        1        0
   week_7   week_6   week_5   week_4   week_3   week_2   week_1
   (MSB)                                              (LSB)
```

| Bit | Value | UI control | Label resource |
|-----|-------|------------|----------------|
| 0 (LSB) | 1 | `week_one` | `multialarm_1` |
| 1 | 2 | `week_two` | `multialarm_2` |
| 2 | 4 | `week_three` | `multialarm_3` |
| 3 | 8 | `week_four` | `multialarm_4` |
| 4 | 16 | `week_five` | `multialarm_5` |
| 5 | 32 | `week_six` | `multialarm_6` |
| 6 (MSB) | 64 | `week_seven` | `multialarm_7` |

How the app builds the string (`changeWeekView` in `MultiAlarmSettingActivity`):

```text
for i from 6 down to 0:
    append '1' if week[i] selected else '0'
```

Examples:

| Selection | Binary string | Integer |
|-----------|---------------|---------|
| Only `week_one` | `0000001` | 1 |
| Only `week_seven` | `1000000` | 64 |
| Everyday | `1111111` | **127** (`0x7F`) |
| One-shot (no weekdays) | `0000000` | **0** |

**Special values**

| Value | Meaning |
|-------|---------|
| `0` | **One-shot** — use date in bytes `[8–11]`, not weekly repeat |
| `1` … `126` | Selected weekdays |
| `127` (`0x7F`) | Every day |

When loading UI state, the binary string is **reversed** so `charArray[i]` lines up with `mWeekTextList[i]` (`week_one` … `week_seven`). Display mapping in the list activity: string index `0` → `multialarm_7`, index `6` → `multialarm_1`.

**Weekday labels:** `multialarm_1` … `multialarm_7` come from string resources (typically Mon…Sun in this app family). Confirm against the device locale if strict day mapping matters.

#### One-shot date (`getDateByte`)

When `repeat == 0` and date ≠ `0000-00-00`:

```text
[8–9] year (two bytes derived from hex of year)
[10]  month
[11]  day
```

### Scene (`byte[7]`)

Not a bitfield — **index into the scene icon grid** (`0`, `1`, `2`, …).

Theme packs (`MULTI_ALARM_TYPE` / `SpUtil`):

| Type | Pack |
|------|------|
| 1 | JiaMei icons |
| 2 | GongBan icons |
| 3 | GongBan2 icons |
| 4 | Unselected / scene UI hidden |

Protocol only carries the integer; icon meaning is pack-specific.

### Practical write examples

**Weekdays bits 0–4 only** (if those are Mon–Fri in the locale):

```text
mask = 1+2+4+8+16 = 31 = 0x1F
B9 01 <id> <H> <M> 01 1F <scene> 00 …
```

**Every day:**

```text
B9 01 <id> <H> <M> 01 7F <scene> 00 …
```

**One-shot** (date filled by `getDateByte`):

```text
B9 01 <id> <H> <M> 01 00 <scene> <yr0> <yr1> <month> <day> …
```

### Reply (read stream)

Multi-packet. Fields used when saving:

| Offset | Meaning |
|--------|---------|
| 1 | Special: `3` = re-read request from device |
| 2 | Current packet index |
| 3 | Total packets |
| 4 | Status discriminator (data vs clear result) |
| 5 | Alarm ID |
| 6 | Hour |
| 7 | Minute |
| 8 | Open |
| 9 | Repeat bitmask |
| 10 | Scene |
| 11–12 | Year (endian-swapped hex concat) |
| 13 | Month |
| 14 | Day |

### Classic 3-alarm (`0xAB`) note

Classic alarms have **no** weekday mask or scene — only hour, minute, open for three fixed slots (see §17).

**Source:** `MultiAlarmHandler.getAlarmCmd()` / `getRepeartStatusStr()` / `handlerMultiAlarm()` / `saveMultiAlarm()`, `MultiAlarmBean`, `MultiAlarmSettingActivity.changeWeekView()` / `onItemClick()`.

---

## 19. Countdown (`0xB2`) — full layout

**Header:** `0xB2` (`HEAD_COUNT_DOWN`)

### Write

| Offset | Meaning |
|--------|---------|
| 0 | `0xB2` |
| 1 | Operate: **`0` = cancel**, **`1` = set**, **`2` = read** |
| 2 | Countdown ID |
| 3 | Seconds byte 0 (low) |
| 4 | Seconds byte 1 |
| 5 | Seconds byte 2 (from `intToBytes` indices 3,2,1 → 24-bit) |
| 6 | UI mode: `0` = watch UI off, `1` = watch UI on, `2` = not test-by-watch |

Cancel / read may omit full seconds payload.

### Reply

| Offset | Meaning |
|--------|---------|
| 1 | Status: `1` = setting, `2` = read, `3` = running (COUNT_ING), `4` = ended |
| 2 | `0` = operate failed; non-zero = success |
| 3 | Countdown ID |
| 4–6 | Default / configured seconds (parsed as big-endian hex of [6][5][4]) |
| 7 | Open watch UI (`!= 0`) |
| 8–10 | App-side remaining seconds |
| 11 | Count-down by watch (`1` = yes) |

**Source:** `CountDownHandler.getBytes()` / `handler()`, `CountDownBean` / `CountDownBackBean`.

---

## 20. Screen style (`0xC7`) — full layout

**Header:** `0xC7` (`HEAD_READ_SCREEN_STYLE`)

### Write / read

| Offset | Set | Read |
|--------|-----|------|
| 0 | `0xC7` | `0xC7` |
| 1 | `0x01` | `0x02` |
| 2 | **Style ID** (index to apply) | — (zeroed) |

**Set style N:** `C7 01 <N> 00…`  
**Read:** `C7 02 00…`

### Reply

| Offset | Meaning |
|--------|---------|
| 1 | `1` = set result, `2` = read result |
| 2 | Success `1` / fail `0` |
| 3 | Current style ID |
| 4 | Style type |
| 5–19 | Five packed watch-UI ID values (3 bytes each, custom bit packing into ints) |

**Source:** `ScreenStyleHanlder.getScreenStyleCmd()` / `getScreenStyleStatus()`, `UiStyleDevice`.

---

## 21. Settings coverage matrix (tool-oriented)

| Setting | Header | Documented | Notes |
|---------|--------|------------|-------|
| Notification switches (call/SMS/apps) | `0xAD` | §13 | |
| Message wakes screen | `0xB8` pkg2 | §14 | |
| Global screen-on duration | `0xB4` | §10.5 | |
| Brightness schedule | `0xB1` | §16 | |
| Screen style | `0xC7` | §20 | |
| Classic 3 alarms | `0xAB` | §17 | |
| Multi-alarm | `0xB9` | §18 | |
| Countdown | `0xB2` | §19 | |
| Night turn-wrist | `0xAA` | §6.4 | |
| Check-wear | `0xE2` | §6.5 | |
| Long-seat | `0xE1` | §6.3 | |
| Person info | `0xA3` | §6.7 | |
| Language | `0xF4` | §6.8 | |
| Weather | `0xC8` | §6.6 | |
| Women / menses | `0x85` | §10.1 | |
| SpO₂ breath-break remind | `0x86` | §10.6 | |
| All-settings / night SpO₂ | `0xB3` | §10.7 | |
| Person watch toggles | `0xB8` pkg1 | §14 | |


---

## 22. Heart-rate warning (`0xAC`)

**Header:** `0xAC` (`HEAD_HEART_WARING`)

### Write (set thresholds + enable)

| Offset | Meaning |
|--------|---------|
| 0 | `0xAC` |
| 1 | High HR threshold (bpm) |
| 2 | Low HR threshold (bpm) |
| 3 | Open: `1` = ON, `0` = OFF |

**Read request:** `AC 00 00 02` (`HEART_WARING_READ`)

### Reply

| Offset | Meaning |
|--------|---------|
| 1 | High threshold |
| 2 | Low threshold |
| 3 | Status (e.g. `16` = unsupported; with [4] forms open/close success/fail) |
| 4 | Operate discriminator |
| 5 | Open flag |

App clamps high threshold to minimum 70 when saving.

**Source:** `HeartWarningHandler.getHeartWaringCmd()` / `handler()`.

---

## 23. Find watch by phone (`0xAE`)

**Header:** `0xAE` (`HEAD_FIND_WATCH_BY_PHON`)

| Command | Bytes |
|---------|-------|
| Open (start find / anti-lost alert on watch) | `AE 01` |
| Close | `AE 00` |

### Reply

| Offset | Meaning |
|--------|---------|
| 1 | Success `1` / fail `0` |
| 2 | `1` = open result, `0` = close result; other = read |

**Source:** `FindWathchByPhoneHanlder`, `BleProfile.FIND_WATCH_BY_PHONE_OPEN/CLOSE`.

---

## 24. Continuous heart rate (`0xD0`)

| Command | Bytes |
|---------|-------|
| Start | `D0 01` |
| Stop | `D0 00` |

(`RATE_CURRENT_READ` / `RATE_CURRENT_CLOSE`)

Live values arrive as notifications on the same header (exact value layout follows continuous HR path in `HeartHandler` / original data).

---

## 25. BP model setting (`0x91`)

**Header:** `0x91` (`HEAD_BP_MODEL`)

| Offset | Meaning |
|--------|---------|
| 0 | `0x91` |
| 1 | Mode: `0` = normal, `1` = private, `2` = read, `3` = cancel dynamic calibrate |
| 2 | High value (for private / calibrate) |
| 3 | Low value |
| 4 | `1` if dynamic-adjust flag set |

Fixed helpers in profile:
- Normal: `91 00 00 00`
- Private: `91 01 00 00`
- Read: `91 02 00 00`

**BP measurement** uses header `0x90`:
- Start normal: `90 01 00`
- Start private: `90 01 01`
- Stop normal: `90 00 00`
- Stop private: `90 00 01`

**Source:** `BPSettingHandler.getBpCmp()`, `BleProfile` BP arrays.

---

## 26. Weather status (`0xC8`) — enable / type

**Header:** `0xC8` (`HEAD_WEATHER`)

Sub-ops used by `WeatherHandler`:
- `1` = content setting (multi-packet forecast push via `WeatherHandlerPackage`)
- `2` = status read
- `3` = status set (on/off + type)

### Status set

| Offset | Meaning |
|--------|---------|
| 0 | `0xC8` |
| 1 | `0x03` (status set) |
| 2 | isOpen (`0`/`1`) |
| 3 | weather type (low byte) |

### Status read

`C8 02 …`

### Status reply

| Offset | Meaning |
|--------|---------|
| 1 | Sub-op echo |
| 2 | Success / fail |
| 3–4 | CRC (16-bit, endian-concat) |
| 5 | isOpen |
| 6 | weather type |

**Forecast content** is a multi-packet stream built by `WeatherHandlerPackage.getWeatherSendList()` (city/time/temp/condition blocks under the same `0xC8` header with content sub-op). Full per-byte weather content layout is firmware/app-version specific; status on/off above is sufficient to enable the feature.

---

## 27. Updated settings coverage matrix

| Setting | Header | Section |
|---------|--------|---------|
| Notification switches | `0xAD` | §13 |
| Message wakes screen | `0xB8` pkg2 | §14 |
| Global screen-on duration | `0xB4` | §10.5 |
| Brightness schedule | `0xB1` | §16 |
| Screen style | `0xC7` | §20 |
| Classic 3 alarms | `0xAB` | §17 |
| Multi-alarm | `0xB9` | §18 |
| Countdown | `0xB2` | §19 |
| Heart-rate warning | `0xAC` | §22 |
| Find watch | `0xAE` | §23 |
| Continuous HR | `0xD0` | §24 |
| BP model / measure | `0x91` / `0x90` | §25 |
| Weather status | `0xC8` | §26 |
| Night turn-wrist | `0xAA` | §6.4 |
| Check-wear | `0xE2` | §6.5 |
| Long-seat | `0xE1` | §6.3 |
| Person info | `0xA3` | §6.7 |
| Language | `0xF4` | §6.8 |
| Women / menses | `0x85` | §10.1 |
| SpO₂ breath-break | `0x86` | §10.6 |
| Night SpO₂ / all-setting | `0xB3` | §10.7 |
| Person watch toggles | `0xB8` pkg1 | §14 |


---

## 28. Contacts (`0x72`) — full layout

**Header:** `0x72` (`HEAD_CONTACT` = 114)

### Operations (byte[1])

| Op | Meaning |
|----|---------|
| `1` | Write / push contact list (multi-packet) |
| `2` | Read (CRC in [2–3]) |
| `3` | Move contact (fromId, toId) |
| `4` | Delete by ID (see below) |

### Write stream (op = 1)

Each BLE packet:
```
[0]  0x72
[1]  0x01
[2]  current package (1-based)
[3]  total packages
[4–19] 16-byte payload chunk
```

Payload is a concatenation of **TLV contact records** (from `ContactPackage.getByteArrFormContact`):

```
[0]     0xA0                    record tag
[1–2]   record length (LE, includes these 3 header bytes)
[3…]    fields:
          type 0xA1, len=1, contactId (1 byte)
          type 0xA2, len=N, nickname UTF-8 (max ~20 chars)
          type 0xA3, len=N, telephone UTF-8 (max ~22 chars)
```

TLV helpers (`ConVertyUtil`):
- **Int type1:** `{type, 0x01, value}`
- **String:** `{type, len, …utf8 bytes}`

*(Decompiler maps A1/A2/A3 through R2.attr; sequential tags `0xA1/A2/A3` after `0xA0` match naming and weather-style F0 tagging.)*

### Read
```
72 02 <crc_lo> <crc_hi> 00…
```

### Move
```
72 03 <fromId> <toId> 00…
```

### Delete
```
72 04 <deleteId> 00…   (exact second byte from getDeleteContactList)
```

**Source:** `ContactPackage`, `ContactHandler`, `ConVertyUtil`.

---

## 29. GPS data (`0x70`) — full layout

**Header:** `0x70` (`HEAD_GPS_DATA` = 112)

### Phone → watch control

| Op (byte[1]) | Meaning |
|--------------|---------|
| `1` | Start / request CRC list |
| `3` | Read sport data for one CRC: `[2]=crc_lo`, `[3]=crc_hi` |

### Watch → phone stream (`GpsDataOprate.handleByte`)

| byte[1] | Meaning |
|---------|---------|
| `1` | CRC list packet (`[2]==1` starts a new list) |
| `2` | CRC list complete → app calls `readFirstData()` |
| `3` | Sport/track data packet (`[2]==1` starts list) |
| `4` | Sport data complete → parse, then `readNextData()` for next CRC |

### Coordinate encoding (`GpsConvertUtil`)

Longitude/latitude stored as **absolute value × 10⁹** in **5 bytes**, with sign bits in a flag byte:

```
slice[0] bit4: lon sign (0 = +, 1 = −)
slice[0] bit3: lat sign (0 = +, 1 = −)
slice[1..5]:  |lon| * 1e9  (5-byte big integer via CUtil.getFiveByteToInt)
slice[6..10]: |lat| * 1e9
```

Reconstructed degrees:
```
lon = sign_lon * fiveByteToInt(bytes[1..5]) / 1e9
lat = sign_lat * fiveByteToInt(bytes[6..10]) / 1e9
```

### Live GPS info packet (phone→watch style from `getGpsInfoByteArr`)

| Offset | Meaning |
|--------|---------|
| 0 | `0x70` |
| 1 | GPS sport state |
| 2 | GPS state |
| 3–4 | Distance × 10 (LE uint16) |
| 5 | Info head flags |
| 6–10 | |lon| × 1e9 (5 bytes) |
| 11–15 | |lat| × 1e9 (5 bytes) |

**Source:** `GpsDataOprate`, `GpsConvertUtil`, `CUtil.getFiveByteToInt`.

---

## 30. Weather content push (`0xC8` sub-op 4) — multi-packet

Builds on §26 status control.

### Content packets

```
[0]  0xC8
[1]  0x04          content transfer
[2]  total packages
[3]  current package (1-based)
[4–19] 16-byte payload
```

Payload is a flat byte stream from `WeatherHandlerPackage.getByteArrFormWeather(WeatherBeanKt)`:
- City / location / time (`TimeBeanJson`)
- Daily forecasts (`WeatherEveryDay`: high/low, condition codes)
- 3-hour forecasts (`WeatherEvery3Hour`)
- CRC field on the bean

Validity helpers:
- 3-hour block valid if within 180 minutes of “now”
- Day block valid if date ≥ today

Full per-field TLV inside the F0 blob is version-specific; framing above is stable for clients that only need to enable weather (§26) or mirror a captured content stream.

**Source:** `WeatherHandlerPackage.getWeatherSendList()` / `getByteArr()`, `WeatherBeanKt`.

---

## 31. HID bind (`0xBC`)

**Header:** `0xBC` (`HEAD_HID_BIND` = -68)

```
BC 01    // HID_BIND — request / enable HID pairing path
```

Used with the device’s HID profile for media keys / remote control. No multi-byte payload in the profile constant; further state is device-side.

---

## 32. Language (`0xF4`)

**Header:** `0xF4` (`HEAD_CHANGE_WATCH_LANGUAGE`)

| Command | Bytes |
|---------|-------|
| Chinese | `F4 00 00 00 00` |
| English | `F4 00 00 00 01` |

(`CHANGE_WATCH_LANGUAGE_ZH` / `_EN`)

---

## 33. TLV helper summary (`ConVertyUtil`)

Used by contacts (and similar big payloads):

| Helper | Layout |
|--------|--------|
| `getIntAndType1` | `{type, 0x01, value8}` |
| `getIntAndType2` | `{type, 0x02, value_lo, value_hi}` |
| `getStringAndType` | `{type, len, …UTF-8}` |
| `getTimeAndType` (day) | `{type, 0x03, year-2000, month, day}` |

Generic multi-packet framing (contacts, weather content):
```
[0] header
[1] content op
[2] total (or index — order varies by feature)
[3] index (or total)
[4–19] 16 bytes payload
```
Weather uses `[2]=total, [3]=index`; contacts use `[2]=index, [3]=total`.


---

## 34. Music metadata push (`0x99`) and watch→phone control (`0x01`)

### Phone → watch: now-playing info (`HEAD_MUSIC` = `0x99` = -103)

Multi-packet content (same 16-byte chunk style as weather/contacts):

```
[0]  0x99
[1]  0x01
[2]  package index (1-based)
[3]  total packages
[4–19] 16-byte payload
```

Payload is a **`0xF0` wrapper** + concatenated TLVs:

```
F0 <total_inner_len>
  album:  type 0xA0, len, UTF-8   (HEAD_READ_BATTERY)
  title:  type 0xA1, len, UTF-8   (HEAD_PWD)
  artist: type 0xA2, len, UTF-8   (OAD_CMD_HEAD)
  status: type 0xA3, 0x01, playStatus
  volume: type 0xA4, 0x01, voiceLevel
```

(`MusicHandler.getMusicContentByte` / `getMusicByteList`)

### Watch → phone: media keys (`0x01` auto-callback)

```
[0]  0x01
[1]  0x01          music subtype
[2]  0x01          must be 1
[3]  action:
       1 = next track
       2 = play / pause
       4 = previous track
```

App maps these to Android media keycodes 87 / 85 / 88 (`AutoCallbackHandler.musicOprate`). Debounced to ≥1 s.

PTT state uses the same header with `[1]=2`, `[3]=1` open / `2` close.

**Source:** `MusicHandler`, `AutoCallbackHandler`, `BleProfile.HEAD_BATTERY_AUTO_CALLBACK_MUSIC`.

---

## 35. ECG / PTT app control (`0x93`)

**Header:** `0x93` (`HEAD_ECG_DATA_APP` = -109)

### Control write

```
[0]  0x93
[1]  0x01
[2]  mode:
       1 = start ECG
       2 = stop ECG
       3 = start PTT/FTT
       4 = stop PTT
[3]  notify flag: 1 = notify on, 0 = off
```

Profile helpers:
- `READ_ECG_NOTIFY` / `READ_ECG_UNNOTIFY`
- `READ_FTT_NOTIFY` / `READ_FTT_UNNOTIFY`

### Stream types (watch → phone)

**Start packet** (`handlerTypeStart`):  
`[4–5]` and `[6–7]` form two 16-bit values (endian-concat of hex pairs) — session / scale params.

**Middle / sample packet** (`handlerTypeMiddle` → `EcgAppDetect`):  
bytes `[1]…[13]` sample fields, `[14–15]` 16-bit value, `[19]` progress/flags.

**End / result packet** (`handlerTypeEnd` → `EcgResult`):
| Offset | Meaning |
|--------|---------|
| 4 | Lead sign |
| 5–12 | Result8 array (8 values) |
| 13 | Average heart rate |
| 14 | Average respiratory rate |
| 15 | Average HRV |
| 16–17 | Average QT (16-bit) |
| 19 | Progress |

Related IDs: `HEAD_ECG_DATA_GET_ID` (`0x96`), `HEAD_ECG_DATA_USE_ID` (`0x97`) for stored ECG record selection.

**Source:** `EcgAppHandler`.

---

## 36. UI / big-data transfer (`0x50` + `0xCC`)

Used for watch-face / UI binary update.

### Base info (`HEAD_BIG_DATA_TARN` = `0x50`)

**Read:**
```
50 02 01 00…
```
(`read=2`, `useTypeUI=1`)

**Set (enter UI transfer):**
```
50 01 01
  [3–6]   data receive address (LE u32)
  [7–10]  file length (LE u32)
  [11–12] bin data type (LE u16)
  [13–14] image CRC id (LE u16)
  [15]    0x01
```

Reply path: `[1]==1 && [3]==1` → entered UI mode; `[1]==2 && [3]==1 && [2]==1` → UI data info.

### Content blocks (`HEAD_BIG_DATA_SEND_CONTENT` = `0xCC`)

```
[0]  0xCC
[1]  0x06
[2–3] block index (LE, 1-based)
[4–5] CRC-16 of this block payload
[6–7] payload length (LE)
[8…]  up to 12 bytes of file data in first form;
      UIUpdateHandler also splits larger blocks into 20-byte raw slices
```

Watch ACK on `0xCC` (via BluetoothService UI path):
- `[1]==1` → block OK, send next
- `[1]==2` → CRC mismatch, retransmit
- `[1]==3` → flash write CRC fail, retransmit  
`[2–3]` block id, `[4–5]` CRC echo.

**Source:** `BigDataHandler`, `UIUpdateHandler`, `BluetoothService` UI block state machine.

---

## 37. Weather day model fields (for content builders)

`WeatherEveryDay` carries (when building §30 content):
- `temperatureMaxC` / `temperatureMinC` / `MaxF` / `MinF`
- `weatherStateWhiteDay` / `weatherStateNightDay` (condition code ints)
- `yellowLevel`, `windLevel`, `canSeeWay`
- `timeBean` (date)

Exact numeric condition-code table is not fully expanded in the APK strings (likely mapped from a weather API → device icon index). Clients that only toggle weather (§26) do not need the table; full content push requires capturing a live stream or matching the API mapping used by the official app.

---

## 38. Protocol coverage status

| Area | Status |
|------|--------|
| Auth, GATT, notifications C1/C2/AD | Complete |
| Screen B1/B4/B8/C7 | Complete |
| Alarms AB/B9, countdown B2 | Complete |
| Contacts 0x72, GPS 0x70 | Complete (TLV + fixed-point) |
| Music 0x99 + 0x01 keys | Complete |
| ECG/PTT 0x93 | Control + result layout complete; raw ADC sample scaling device-dependent |
| UI transfer 0x50/0xCC | Base info + block ACK complete |
| Weather status + framing | Complete; inner condition codes partial |
| Women, drink, long-seat, HR warning, BP, language, find watch | Complete |

