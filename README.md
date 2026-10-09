# S226 for Linux

Linux tools for the **S226 fitness watch** (H-Band / Veepoo), based on a
reverse-engineered Bluetooth LE protocol. No phone and no H-Band app are
needed.

- **`s226-hr`**: Qt GUI with tabs for live heart rate, settings, alarms,
  notifications (including now-playing), activity history, sleep and workouts.
  Optional metronome and system-tray support.
- **`s226-cli`**: the same features in a terminal — heart rate or blood
  pressure, settings, alarms, messages, music metadata, history, sleep,
  workouts, and raw protocol access.
- **`s226ble`**: the C++20 library both are built on (libusb + minimal
  HCI/L2CAP/ATT host; no BlueZ).
- Python research tools and the protocol notes in [PROTOCOL.md](PROTOCOL.md).

```bash
nix run .#                      # s226-hr
nix run .#s226-cli -- --list    # show usable Bluetooth controllers
nix run .#s226-cli              # heart rate in the terminal (-v: protocol log, --bp: blood pressure)
```

Wake the watch by pressing its button so that it advertises; the tools
connect as soon as they see it.

## s226-hr

The window is organised as **tabs**. The toolbar always has controller
selection, watch address and Connect.

**Live** (default tab)

- **Heart rate** as a big number that scales with the window. The watch
  ends a measurement after ~30-50 s; the app restarts it automatically.
- **Steps today** and **cadence** in steps per minute (spm). The cadence
  is measured between the moments the watch's step counter changes
  (polled every second), so it follows the watch's own update rate and
  drops to 0 when you stop.
- **Graph** of bpm and spm over 1 min to 2 h (pick the span; hover for
  exact values).
- **Log**: every reading is appended to one CSV file per day,
  `~/.local/share/s226/s226-hr/s226-hr-YYYY-MM-DD.csv` (columns
  `time,bpm,spm`). The graph is refilled from it on restart.
- **Metronome** that clicks on every beat; the heart icon pulses along
  either way.
- **Blood pressure** measurement on demand.
- The status bar shows the watch's **battery** (hover for the firmware
  version).

**Settings** — sedentary reminder, heart-rate alarm, screen-on time,
brightness, countdown preset, watch face, personal data, feature toggles
(metric, 24h, auto-HR, stopwatch, …) and which message types the watch
shows. Refresh / Apply.

**Alarms** — list, add, edit and delete the alarms stored on the watch.

**Notify** — send a text message (with type), show an incoming-call
screen, or push now-playing metadata; shows media keys from the watch.
Optional MPRIS forwarding maps those keys to the system media player,
and **From player** fills the form from the active player.

**History** — fetch the watch's 5-minute activity slots for today or a
previous day.

**Sleep** — sleep sessions (deep/light minutes, quality, stage curve).

**Workouts** — sport-mode sessions with per-minute detail.

Connects on start (`--no-connect` to skip) and reconnects when the
watch drops out. Connects to any S226 by default; pick or type an
address in the **Watch** field (or pass `--address`). Watches you have
connected to before are listed there.

Closing the window hides to the **system tray** when one is available
(tooltip shows connection state and latest bpm). Restore from the tray
icon; **Quit** in the tray menu or **Ctrl+Q** exits fully.

Keys: **F11** / **Esc** full screen, **M** metronome, **Ctrl+L** log
panel, **Ctrl+Q** quit. Details are in `man s226-hr`.

## s226-cli

Without a command it prints the heart rate like `s226-hr`. Commands run in
order after connecting, print to stdout, and exit:

```bash
s226-cli --info --settings              # firmware, battery, settings
s226-cli --history=1 > yesterday.csv    # 5-minute slots (steps, HR, ...)
s226-cli --sleep=0                       # last night's sleep sessions
s226-cli --weather on                    # enable weather status
s226-cli --contacts 'Ada:+1555,Bob:+1566'  # push contacts
s226-cli --workouts                     # sport-mode sessions, per minute
s226-cli --alarms                       # list alarms on the watch
s226-cli --alarm 1=07:30/mon,tue,wed,thu,fri
s226-cli --sedentary 08:00-18:00/60 --hr-alarm 50-115 --screen-time 10
s226-cli --person 175,60,34,m,9000      # height, weight, age, sex, step goal
s226-cli --notify "Tea is ready"        # message on the watch
s226-cli --messages call,sms,other --notify-type sms --notify "Hi"
s226-cli --call "Alice"                 # incoming-call screen
s226-cli --music "Title/Artist/Album"   # now-playing on the watch
s226-cli --feature music=on             # enable music control if needed
s226-cli --feature stopwatch=off        # other features: see --settings
s226-cli -v --send "a0 00"              # raw command, prints the replies
```

While connected without a command, media keys from the watch print as
`music: next`, `music: play-pause` or `music: previous`.

Distance and calories are left out of the Live graph on purpose: the
watch reports them, but they are its own estimates, not measurements.
History and workouts still show the values the watch stores.

## Installing

The package installs `s226-hr` and `s226-cli` with their desktop
integration under the app ID `s226-hr` (the binary name, not a
reverse-DNS ID):

- a `.desktop` launcher with "Start in Full Screen" and "Start Without
  Connecting" actions
- an SVG icon plus PNG sizes
- AppStream metadata
- the `s226-hr(1)` and `s226-cli(1)` man pages
- a udev rule for device access (see below)

On NixOS, put the flake's package in `environment.systemPackages`
(launcher, icon, man pages) and `services.udev.packages` (device
access). With plain CMake (needs Qt 6 with Multimedia and Svg, and
libusb):

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

### Permissions

The tools drive a USB Bluetooth controller directly, so the user needs
read/write access to its device node `/dev/bus/usb/BBB/DDD`.
`udev/70-s226-bluetooth.rules` grants that to the logged-in user for
every USB Bluetooth controller. On NixOS, `services.udev.packages`
installs it.

## Picking the Bluetooth dongle

The tools talk to the controller over raw HCI (like
[Bumble](https://github.com/google/bumble)), so BlueZ cannot interfere.
While they run, the kernel's `btusb` driver is detached from that
controller (its `hciN` adapter disappears) and re-attached on exit.
`bluetoothd` does not need to be stopped, but anything else using that
adapter loses it in the meantime.

Controllers are found by their USB Bluetooth interface (class e0/01/01),
not by name. That matters for WiFi/Bluetooth combo chips such as
`0bda:b82c`, which calls itself "802.11ac NIC". Only the Bluetooth
interface is taken over; WiFi on the same chip keeps working.

`s226-cli --list` prints what is available. Wherever a controller can be
chosen (`--controller` in both tools, the drop-down in the GUI), any of
these work:

| Selector | Example |
|----------|---------|
| automatic (default) | `auto` |
| index from `--list` | `0` |
| vendor:product | `0bda:b82c` (`usb:0bda:b82c` also accepted) |
| USB port path | `1-10.1` |
| bus/device as in `lsusb` | `001/012` |
| kernel adapter name | `hci0` |

**Realtek dongles** need the patch firmware that the kernel loads when
the dongle is plugged in; it survives the takeover. If the log warns
about "unpatched" firmware, replug the dongle and try again.

Range can be short: early tests only found the watch about 10 cm from
the dongle. If connecting is slow, bring the watch closer.

## Protocol in short

All application traffic goes through the vendor service `f0080001-…`:
commands are Write Commands to `f0080003`, and replies are notifications
on `f0080002`. No Bluetooth pairing is involved. After enabling
notifications, the host must send the `0xA1` bind packet (which also
sets the watch clock) right away.

| Command | Purpose | Watch reply |
|---------|---------|-------------|
| `a1 00 00 00 <time> 01 01 04 …` | bind, set time | `a1 …` with the MAC address |
| `d0 01` / `d0 00` | heart rate start / stop | `d0 <bpm> …` about once per second |
| `90 01 00` / `90 00 00` | blood pressure start / stop | `90 00 00 <pct>` progress, `90 <sys> <dia> 64` result |
| `d8 00` | poll today's totals | `d8 00 <steps> <distance> <calories>` (u32 LE) |
| `d1 01 00 0N` | daily history of day N | one frame per 5-minute slot |
| `e0 0N` | sleep for day N (0 = last night) | multi-frame; index 0 ends the day |
| `d4 0N` | workout in slot N (1-3) | header frames, then one per minute |
| `a0 00` | battery | `a0 00 <0x80\|percent> 00 <bars>` |
| `e1`, `ac`, `b4`, `a3` | sedentary reminder, HR alarm, screen-on time, personal data | settings echo |
| `b9` | alarms read / write / delete | alarm frames |
| `99` | now-playing metadata (phone → watch) | (display on watch) |
| `01 01 01 <n>` | media key (watch → phone): next / play-pause / previous | — |

[PROTOCOL.md](PROTOCOL.md) has the full layouts, the captures they come
from, and the remaining unknowns.

## Development

```bash
nix develop
cmake -B build && cmake --build build && ctest --test-dir build
```

The tests cover the protocol decoders and cadence calculation against
frames from the phone capture, and validate the `.desktop` file,
AppStream metadata and man pages.

| Path | What |
|------|------|
| `lib/` | `s226ble` library, plain C++20 + libusb, no Qt |
| `lib/include/s226/protocol.hpp` | S226 packet builders and decoders (pure functions) |
| `lib/include/s226/watch.hpp` | `s226::Watch`: scan, connect, bind, measurements, `send`, events |
| `lib/include/s226/usb.hpp` | USB Bluetooth controller discovery and selectors |
| `lib/include/s226/step_rate.hpp` | cadence from the step counter |
| `lib/src/` | private: libusb HCI transport, minimal LE host (HCI, L2CAP, ATT) |
| `app/` | Qt 6 GUI (`s226-hr`): tabs, WatchBridge, system tray |
| `cli/` | `s226-cli` |
| `data/`, `man/`, `udev/` | desktop integration, man pages, udev rule |

## Python research tools

Two older Python tools remain for protocol experiments.

**`s226_bumble.py`** (`nix run .#s226-bumble`) uses
[Bumble](https://github.com/google/bumble) over raw HCI. It needs the
dongle to itself: run it with `sudo` or stop `bluetooth` first.

```bash
sudo nix run .#s226-bumble -- --transport usb:0bda:b82c --hr        # live heart rate
sudo nix run .#s226-bumble -- --transport usb:0bda:b82c --probe bp  # blood pressure
sudo nix run .#s226-bumble -- --transport usb:0bda:b82c -v --listen 60
```

Other flags: `--probe NAME` (`sync`, `hr`, `hr-stop`, `bp`, `bp-stop`,
…; repeatable), `--hr-duration SEC`, `--address ADDR`, `--no-auth`,
`--no-notify`, `--notify-after-a1`, `--scan-timeout SEC`.

**`s226.py`** (`nix run .#s226`) goes through BlueZ with
[Bleak](https://bleak.readthedocs.io/). It can scan, connect, dump the
GATT database and send the bind packet. However, `bluetoothd` starts SMP
pairing because the watch exposes a HID service, and that tears down the
link, so notifications do not work through BlueZ. That is why the other
tools use raw HCI.

```bash
nix run .#s226 -- --scan-only --scan-timeout 120   # log advertisements
nix run .#s226 -- --scan-timeout 300 --listen 60 --log-file s226-$(date +%Y%m%d-%H%M%S).log
```

Other flags: `--adapter`, `--address`, `--connect-timeout`,
`--connect-retries`, `--no-gatt-dump`, `--read-values`, `--no-auth`,
`--enable-notify`, `--all-notify`.

## Reverse-engineering notes

### Hardware

| | |
|---|---|
| Device | S226, 1.3" 240×240 display |
| MCU | Nordic nRF52832, Bluetooth LE 4.0 |
| Firmware | 31.06 (H-Band shows `89:ab 851 01.31.06`) |
| Companion app | H-Band (`com.veepoo.hband`) |

### Advertising and address

- The watch does **not advertise continuously**: it advertises in short
  bursts, mostly after it has been woken. A tool has to connect as soon
  as it sees an advertisement; scanning first and connecting later
  misses it.
- It uses a **random** address, e.g. `C1:23:45:67:89:AB`. The watch and
  H-Band show its last two bytes (`89:AB`).
- **Manufacturer data** has company ID `0xF8F8` and a payload that is the
  address reversed (`ab 89 67 45 23 c1`), which identifies the watch
  even without its name.
- Some advertisements carry only 19 bytes of service data for UUID
  `0xFCF1` and use a different address; their meaning is unknown.

### GATT layout

```text
1800 Generic Access     2a00 name, 2a01 appearance, 2a04 conn params, 2aa6 central addr resolution
1801 Generic Attribute
f0080001 Vendor          f0080002 notify (replies)   f0080003 write (commands)
f0020001 Vendor          f0020002 notify (raw PPG samples while measuring)   f0020003
fee7 (Tencent UUID)      fea1, fea2, fec9
1812 HID                 2a4e, 3x 2a4d report, 2a4b report map, 2a33 boot mouse, 2a4a, 2a4c
```

The company names that BlueZ shows for `fee7`, `fea1`/`fea2` and `fec9`
(Tencent, Intrepid, Apple) are only the Bluetooth SIG assignments of
those UUIDs. They say nothing about who made the watch.

### Open questions

- Meaning of the post-bind `0xA7` dump (`0xAD` and `0xB8` are decoded)
- `0xD1` history bytes 14-16 and 18; `0xD3` reply
- Role of the `fee7` and HID services
