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

## Implementation status (`s226`)

After a successful connect the tool:

1. Prints a **structure-only** GATT dump (no ATT reads by default).
2. Subscribes to `f0080002` and sends the **0xA1** bind packet to
   `f0080003` (`--no-auth` skips this).
3. Subscribes to any remaining notify characteristics.
4. Listens for notifications (`--listen-seconds`).

Use `--read-values` only when investigating; value/descriptor reads
still tend to drop the link.

## Open questions

* Minimal 0xA1 payload (can profile bytes be zero?)
* Meaning of notification opcodes `0xA7`, `0xAD`, `0xB8`, …
* Role of service `f002` and `fee7` / HID
* Whether BLE pairing/bonding is ever required (not seen in this capture)
* Stable handle numbers across firmware versions (prefer UUIDs)

## References

* Phone bugreport HCI: `btsnoop_hci.log.last` inside `s226-bt.zip`
* App: H-Band (`com.veepoo.hband`), Android 5-compatible builds exist
  (e.g. 10.5.x / 10.6.06) when the store build requires Android 6+
