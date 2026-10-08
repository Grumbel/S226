# S226 / Veepoo protocol notes

Findings from reverse engineering the S226 fitness tracker (H-Band /
Veepoo stack) against a real unit and a phone HCI capture
(`btsnoop_hci.log.last`, 2026-08-24).

## Device identity

| Field | Value |
|-------|--------|
| Advertised name | `S226` |
| Address | random static, e.g. `C1:23:45:67:89:AB` (all addresses in these notes are replaced by this example) |
| Manufacturer ID | `0xF8F8` |
| Manufacturer data | 6 bytes = address octets reversed (`ab 89 67 45 23 c1`) |
| Primary adv UUID (idle) | often only `0000fee7-…` |
| Connectable adv | may list GAP, GATT, HID, `fee7`, `f002`, `f008` |

Connect is intermittent: the watch does not always answer link
requests. When the ACL link does come up, **unauthenticated ATT
activity** (descriptor reads, etc.) makes it disconnect within about a
second.

## Vendor GATT (service `f0080001-0451-4000-b000-000000000000`)

Observed handles on this firmware (other builds may renumber):

| Handle | UUID | Role |
|--------|------|------|
| 0x000c | `f0080002` declaration | properties: **notify** |
| **0x000d** | `f0080002` value | **notifications** (watch → phone) |
| **0x000e** | CCCD (`0x2902`) | write `01 00` to enable notify |
| 0x0010 | `f0080003` declaration | properties: write \| write-cmd |
| **0x0011** | `f0080003` value | **commands** (phone → watch) |

There is a second vendor service `f0020001-…` and HID / Tencent
(`fee7`) services; the H-Band session analysed here only used `f008`
for the bind and subsequent commands.

## H-Band post-connect sequence (from HCI)

Timestamps relative to first ATT packet in the capture:

1. **~0–630 ms** — full GATT discovery (Read By Group / Type / Find Info).
2. **~630 ms** — enable notifications:
   ```text
   Write Request  handle=0x000e  data=01 00
   ```
3. **~850 ms** — bind / auth (Write **Command**, no response):
   ```text
   Write Command  handle=0x0011
   a1 00 00 00 07 ea 08 18 10 18 1d 01 01 04 00 00 00 00 00 00
   ```
4. **~880 ms** — several notifications on **0x000d**, including an
   `0xA1` status that embeds the device MAC:
   ```text
   a1 00 00 06 03 53 01 31 06 00 00 01 ab 89 67 45 23 c1 00 01
   ```

Without step 3 the link does not stay useful; Linux attempts that only
dumped GATT and read descriptors died the same way.

## 0xA1 bind packet layout (20 bytes)

```text
offset  content
------  -------
0       0xA1          command
1..3    00 00 00      flags / unknown
4..5    year          big-endian (e.g. 0x07ea = 2026)
6       month
7       day
8       hour
9       minute
10      second
11..13  01 01 04      profile-related bytes copied from H-Band
14..19  00 …          padding
```

This is **not** a bare ASCII `"0000"` password write. H-Band sends
time synchronisation and a few profile bytes in the same PDU. The
minimal subset that the firmware requires is not yet known; the tool
replays the full 20-byte form with the current local time.

## Later commands (same write handle 0x0011)

All of these were **Write Command** in the capture; the watch answered
with notifications on **0x000d** whose first byte often echoes the
command:

| Write payload (prefix) | Notes |
|------------------------|--------|
| `d8 00` | status-style query |
| `f4 02 …` | feature / mode |
| `a0 00` | … |
| `a3 …` | … |
| `e1 …` | … |
| `aa 02 …` | … |
| `91 02 7a 4b …` | … |
| `85 00 …` | … |
| `b1` / `b4` | … |
| `d1` / `d3` / `d4` | multi-notification data sync |

Exact semantics still TBD.

## Connection exclusivity

The S226 appears to allow only **one** active central at a time.

If H-Band (or the phone’s Bluetooth stack) still holds a link — or even a
fresh bond from a recent session — Linux connects will time out while
advertisements continue. Before testing with `s226`:

1. Force-stop H-Band
2. Forget / unpair the S226 in Android Bluetooth settings (optional but helps)
3. Toggle phone Bluetooth off, or leave the phone out of range
4. Power-cycle the watch if connects still time out at strong RSSI

Successful earlier Linux sessions used RSSI roughly −42…−56 with the phone
not connected.


## HCI recheck (btsnoop_hci.log.last)

Re-parsed 2026-08-25:

* **No SMP / pairing** and **no ATT MTU exchange** in the session.
* Exactly one `Write Request`: CCCD handle **0x000e** = `01 00` (~630 ms after first ATT).
* Then `Write Command` to **0x0011** with the 20-byte `0xA1` packet (~853 ms).
* Notifications on **0x000d** within ~30 ms (`0xA7`, `0xAD`, `0xB8`, `0xA1` status with MAC).
* Later probes: `d8 00`, `f4 02…`, `a0 00`, `a3 …`, etc., all Write Command + notify echo.
* HCI disconnect reason on the failed Linux path was **0x16 (Connection Terminated by Local Host)** — BlueZ/Bleak ends the ACL after the ATT Unlikely Error on CCCD, not a spontaneous watch drop after 0xA1.

### Linux behaviour vs phone

| Step | Phone (H-Band) | Linux (Bleak) |
|------|----------------|---------------|
| Service discovery | OK | OK |
| CCCD `01 00` | Write Request OK | `start_notify` → Unlikely Error (0x0e) |
| 0xA1 Write Command | OK after CCCD | OK **before** notify; link stays up |
| Notifications | Many | None (CCCD never enables) |

Default tool path: **0xA1 only** (plus optional write probes). Use `--enable-notify` to retry CCCD.

## Device behaviour (user observations)

The watch is largely **autonomous**. Local UI actions (menus, etc.) stay
on the device and do **not** by themselves generate phone-bound traffic.

What **does** involve the phone / BLE:

| Action | Effect |
|--------|--------|
| Phone connects (H-Band) | Sets **time** on the watch (matches 0xA1 carrying datetime) |
| Phone feature toggles | Enable/disable watch menus (e.g. **stopwatch**) |
| Phone starts **30 s heart-rate** | Watch runs HR session and **streams results to the phone** |

So the interesting protocol surface is **phone-initiated**: time/bind,
config flags, and measurement sessions (HR first). Passive-only use will
not exercise notify traffic.

## Implementation status (`s226`)

On connect the tool:

1. Discovers GATT services.
2. Immediately sends the **0xA1** bind/time packet to `f0080003`
   (`--no-auth` skips this). Happens inside the connect path so the
   write is not lost to a post-connect race.
3. By default **does not** call `start_notify` (`--enable-notify` opts in;
   currently gets ATT Unlikely Error and BlueZ drops the ACL).
4. Sends short write probes (`d8 00`, `a0 00`).
5. Structure-only GATT dump if still connected; optional `--read-values`.
6. Listens for `--listen` seconds.

## Open questions

* How to enable CCCD on `f0080002` from Linux without Unlikely Error
* Minimal 0xA1 payload (can profile bytes be zero?)
* Notification opcodes `0xA7`, `0xAD`, `0xB8`, … once notify works
* Command to **start 30 s HR** and the notify stream format
* Commands for feature flags (stopwatch menu, etc.)
* Role of service `f002` and `fee7` / HID
* Stable handles across firmware (prefer UUIDs)

## Next capture targets (phone HCI)

While H-Band is connected, capture again for:

1. Starting a **30 s heart-rate** measurement (command + notify stream)
2. Toggling **stopwatch** (or similar) on/off in settings
3. Any packet that is not the initial 0xA1 / `d8` / `a0` / `f4` burst

Those will map directly to write probes once CCCD works, or can be
replayed as write-only experiments before that.

## References

* Phone bugreport HCI: `btsnoop_hci.log.last` inside `s226-bt.zip`
* App: H-Band (`com.veepoo.hband`), Android 5-compatible builds exist
  (e.g. 10.5.x / 10.6.06) when the store build requires Android 6+
* Git tip (as of this note): `a1f6288`

---

## Phone HCI capture (Moto G54, 2026-08-25)

Source: `s226-bt.zip` → `FS/data/misc/bluetooth/logs/btsnoop_hci.log`.

Session included heart-rate, blood pressure, alarm/countdown UI, pairing
menu, and automatic time sync on connect. Notifications (Android) were
not exercised.

### Channel

| Role | UUID | ATT handle (this firmware) |
|------|------|----------------------------|
| Notify | `f0080002-0451-4000-b000-000000000000` | 0x000d |
| Write  | `f0080003-0451-4000-b000-000000000000` | 0x0011 |
| CCCD   | 0x2902 on notify char | 0x000e ← `01 00` |

Secondary notify traffic also appears on `f0020002` (handle 0x0015);
primary command plane is **f008**.

### Bind / time (0xA1)

Host → device (Write Command):

```text
a1 00 00 00 | year_be | mon day hour min sec | 01 01 04 00 00 00 00 00 00
```

Device → host (notification), includes MAC:

```text
a1 00 00 06 ... ab 89 67 45 23 c1 ...
                 ^^^^^^^^^^^^^^^^^^^
                 MAC LE = C1:23:45:67:89:AB
```

This is what sets the watch clock on phone connect.

### Post-bind host writes (unique opcodes)

Names come from the H-Band APK's command table (`BleProfile`, headers
as signed Java bytes). The replies are from `btsnoop_hci.log.last`.
Most setting commands share the same layout: `02` in the operation byte
means read, the reply's byte 1 is `01` for OK, and it echoes the stored
values. "✓" means the capture agrees with the APK name.

| Write | APK name | Reply / decoding |
|-------|----------|------------------|
| `d8 00` | current sport | ✓ today's totals, see 0xD8 below (H-Band polls it every few s) |
| `a0 00` | read battery | ✓ `a0 00 <0x80\|percent> 00 <level 0-4>`; `a0 00 cd 00 03` = 77 %, 3 bars (matches the watch) |
| `a3 af 3c 22 01 23 28 01 e0` | person info | ✓ height 175 cm, weight 60 kg, age 34, gender 1, step goal u16 BE 9000, sleep goal u16 BE 480 min; reply `a3 01` |
| `e1 08 00 12 00 3c 02` | long-seat (sedentary) | ✓ start 08:00, end 18:00, every 60 min, op `02`=read, `01` on, `00` off; reply `e1 01 sh sm eh em interval enabled op` (tested on the watch) |
| `ac 00 00 02` | heart warning | ✓ read; write `ac high low on/off`; reply `ac high low enabled op 01`, e.g. `ac 73 34 01 02 01` = on, 52-115 bpm (tested) |
| `aa 02 00 00 01 01` | (not in APK table) | reply `aa 01 02 08 00 16 00 01 05 05` |
| `91 02 7a 4b` | BP model | ✓ private BP calibration 122/75, read; reply `91 01 7a 4b 01 02` |
| `85 00 …` | women / menses | ✓ reply `85 01` |
| `b1 02` | screen light (night dimming) | ✓ read; reply `b1 01 02 00 00 17 3b 01 01 02 0c`: 00:00-23:59, levels |
| `b4 02` | screen-on time | ✓ read; write `b4 01 <sec>`; reply `b4 01 op sec min max 03`: 10 s (range 5-30 s). Out-of-range writes get no reply (tested) |
| `b9 02 … fd 51` | multi-alarm | ✓ read with CRC in the last two bytes; reply `b9 01 00 01 02 …` (no alarms) |
| `ef 01 fe` | battery manager | no reply |
| `72 02 ff ff` | contacts | no reply |
| `f4 02 02 00 01` | change watch language | ✓ reply `f4 01 01 01 02` |
| `e0 00` / `e0 01` | sleep today / yesterday | ✓ empty replies (no sleep was recorded); the old reading as "mode select" was wrong |
| `d1 <first u16 LE> <day>` | original data (5-min slots) | ✓ one frame per slot from `first` to the end of day `day` (0 = today; H-Band asked from 0xC0 = the last slot it had). Any other command except `d8`/`a0` aborts the transfer |
| `d3 01` | sport-mode CRC | ✓ `d3 01 01 00 00 f3 9e …` |
| `d4 <slot>` | sport-mode records | ✓ workout in slot 1-3, see below; an empty slot answers `d4 00 00 00 00 <slot>` |
| `c7 01 0N` | read screen style | — |
| `ae 01` | find watch | `ae 00 …` to every variant tried (`ae 01`, `ae 02`, `ae 01 01`, 20-byte padded); the S226 does not vibrate, so it is probably unsupported |
| `c1 01 01 01` / `c1 01 06 00` / `c1 00 00 00` | SMS alert / incoming call / stop | ack `c1 xx 01`; showed nothing on the S226 (phone/SMS are switched off in `ad`) |
| `c2 <type> <len> <total> <index> <flag> <14 bytes UTF-8>` | message text | ✓ no reply; type 17 ("other") with flag 2 (body) is displayed, multi-packet UTF-8 included. Packets ~120 ms apart |
| `ad 02` | message switches | reply `ad 02` + one byte per message type (byte 2 + type): `01` on, `02` off, `00` unsupported; only type 17 is on here (inferred from what displayed) |
| `d0 01` / `d0 00` | current heart rate | ✓ **Heart rate** start / stop |
| `90 01 00` / `90 00 00` | BP | ✓ **Blood pressure** start / stop |

Status dumps sent right after the bind: `0xA7` = device functions
(feature bitmap), `0xAD` = supported message/notification types, `0xB8` =
watch settings (12/24 h, metric, …). In these the values look like
`1`/`2` = present (on / off) and `0` = not supported, but this is not
verified.

Other headers in the APK table that the S226 capture never exercised:
`ae 01` find watch, `af` disconnect, `b6 01` camera shutter, `b5` find
phone (watch → phone), `b2` countdown, `ab` alarm (old style), `c1`/`c2`
message notification, `c8` weather, `d9` HRV, `80` SpO₂, `81` fatigue,
`82` breathing, `e2` wear check, `93`/`96`/`97` ECG, `99` music.

### Workouts (0xD4) — decoded

`d4 <slot>` returns all frames of one workout recorded in the watch's
sport mode:
`d4 <index u16 LE> <count u16 LE> <slot> <14 payload bytes>`. Frames 1-3
are the header; concatenating their payloads gives:

| Offset | Content |
|--------|---------|
| 0 | `00` |
| 1-7 | start: year u16 LE, month, day, hour, minute, second |
| 8-14 | end, same layout |
| 15-18 | steps, u32 LE |
| 19-22 | distance (m), u32 LE |
| 23-26 | calories (1/1000 kcal), u32 LE |
| 27-30 | activity ("sport value"), u32 LE |
| 31-32 | minutes, u16 LE (= count - 3) |
| 33-41 | unknown (`01 10 0e`, a CRC-like u16 also seen in the `d3` reply, `00 00 ff ff`) |

Then one frame per minute: `hr, activity u16 LE, steps u16 LE, calories
u16 LE (1/1000 kcal), distance u16 LE, flag` (the flag at payload byte 9
is occasionally `01`, maybe a pause). The header totals equal the
per-minute sums exactly (2026-10-06 session: 6189 steps, 5673 m, 371041,
activity 7156 over 177 minutes). The kcal ratio per step matches `d8`
read as 0.1 kcal, which backs that unit too.

### Bind reply (0xA1)

```text
a1 00 00 06 03 53 01 31 06 00 00 01 <MAC reversed> 00 01
         ^^ ^^^^^ ^^^^^^^^
     status  |    firmware 01.31.06
             device number 0x0353 = 851 (H-Band shows "851 01.31.06")
```

According to the APK's reply handler, status is 0 check failed, 1 check
OK, 2/3 set password failed/OK, 4/5 read failed/OK, 6 check OK and time
set. Byte 11 is the wrist-turn function, 18 find-phone and 19 wear
check (0 supported and off, 1 on, 2 not supported).

The APK also describes sleep (`e0`) as multi-frame: `e0 <index, 0 =
last> ? <day>` and 16 payload bytes per frame, split into fixed-size
segments after reassembly. Not verified: this watch had no sleep stored.
Its description of `d1` as a TLV stream does not match the S226, whose
`d1` frames have the fixed layout above.

The APK's description of the bind request matches the capture:
`a1 <pwd u16 BE> <type> <year BE> mon day hour min sec <24h> 01`, with
the default password `0000` sent as the number 0, type `0` = check password
and set time (`1` = change password), and `is_24h = 1`. Byte 13 (`04`)
is not covered by the APK description.

### Heart rate (0xD0) — decoded

```text
host:  d0 01                       start (Write Command, 0x0011)
watch: d0 00 00 00 00 02 ...       ack, measuring
watch: d0 <bpm> 00 00 00 00 ...    ~1 Hz while measuring (0 while settling)
watch: d0 01 00 ... (x2)           watch ended the session (~30-50 s)
host:  d0 00                       stop -> watch acks d0 00 ...
```

Capture: `d0 01` at 17:52:13, then `d0 80` (128), `d0 7f`, ... `d0 7d`
once per second until the watch sent `d0 01` twice ~47 s later. Note the
watch kept streaming for ~20 s after the host `d0 00`.

### Blood pressure (0x90) — decoded

```text
host:  90 01 00                    start
watch: 90 00 00 <pct> 00 01 ...    progress, pct 0..100 (~every 1.26 s)
watch: 90 <sys> <dia> 64 00 01     result at 100 %, e.g. 8f 5f = 143/95
host:  90 00 00                    stop -> watch acks 90 01 00 ...
```

H-Band also writes CCCD `01 00` on handle 0x0016 (`f0020002`) before
`90 01 00`; the watch then streams raw 12-bit PPG samples (u16 LE,
10 per notification) on 0x0015.

### Steps / distance / calories (0xD8) — decoded

The `d8 00` poll H-Band sends every few seconds returns today's totals:

```text
d8 00 | steps u32le | distance u32le | calories u32le | 00 ...
d8 00   ec 1e 00 00   e3 1c 00 00      e4 12 00 00       (2026-08-24 16:24)
        7916 steps    7395 m           4836 (483.6 kcal?)
```

Verified: these equal the sums of the 0xD1 history slots for the same
day up to the same time, field by field. Calorie units are a guess
(0.1 kcal). The 2026-08-25 capture returns all zeros because the watch
logged no steps that day (its history is zero too).

### Daily history (0xD1) — decoded

`d1 01 00 0N` requests day N (0 = today, 1 = yesterday, ...). The watch
answers with one 20-byte frame per 5-minute slot. Note the mixed
endianness:

| Offset | Content |
|--------|---------|
| 0 | `d1` |
| 1-2 | frame index, u16 LE (1-based) |
| 3-4 | frame count, u16 LE (today: slots so far) |
| 5 | hour \| (day offset << 5), e.g. `0x27` = yesterday 07:xx |
| 6-7 | calories, u16 **BE** |
| 8-9 | distance (m), u16 BE |
| 10-11 | steps, u16 BE |
| 12-13 | activity / "sport value", u16 BE (meaning unverified) |
| 14-16 | unknown (14 is set in alternate slots) |
| 17 | heart rate average for the slot |
| 18 | unknown |
| 19 | minute (0, 5, ..., 55) |

The steps column varies independently, while distance and calories keep
a fixed ratio to each other. That is what you would expect if distance
is derived from steps using a cadence-dependent stride, and calories
from distance.

### Bumble status

With raw HCI (`s226-bumble`), CCCD subscribe and 0xA1 succeed. Notify
payloads for 0xA1 / 0xA7 / 0xAD / 0xB8 observed after bind. Probes:

```bash
sudo nix run .#s226-bumble -- --transport usb:0bda:b82c \
  --probe sync --probe stream-b --listen 90
```

### Still open

* Meaning of the trailing status byte in 0xD0 / 0x90 frames
* 0xD1 bytes 14-16 and 18; 0xD3 reply layout; workout header bytes 33-38
* How to write the `ad` switches (to enable call / SMS display)
* Alarm (`b1`) and countdown exact layouts
* Notification (ANCS-style) path — not in this capture
* `f002` secondary channel role
