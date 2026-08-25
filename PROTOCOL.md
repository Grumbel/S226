# S226 / Veepoo protocol notes

Findings from reverse engineering the S226 fitness tracker (H-Band /
Veepoo stack) against a real unit and a phone HCI capture
(`btsnoop_hci.log.last`, 2026-08-24).

## Device identity

| Field | Value |
|-------|--------|
| Advertised name | `S226` |
| Address (this unit) | `FD:32:EF:97:4A:CD` (random) |
| Manufacturer ID | `0xF8F8` |
| Manufacturer data | 6 bytes = address octets reversed (`cd 4a 97 ef 32 fd`) |
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
   a1 00 00 06 03 53 01 31 06 00 00 01 cd 4a 97 ef 32 fd 00 01
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
