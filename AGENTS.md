# S226 — Agent notes

## Project overview

Linux tools for the **S226 fitness watch** (H-Band / Veepoo), based on a
reverse-engineered Bluetooth LE protocol. No phone and no H-Band app are
needed.

- **`s226-hr`**: Qt GUI — tabbed UI for live HR, settings, alarms, notify
  (messages, call, music/MPRIS, desktop notifications), history, sleep and
  workouts; system tray; single-instance by default.
- **`s226-cli`**: Terminal tool for the same features plus weather status,
  contacts push and raw `--send`.
- **`s226ble`**: C++20 library (libusb + minimal HCI/L2CAP/ATT host) used by both.
- Python research tools (`s226.py` via BlueZ/Bleak, `s226_bumble.py` via Bumble raw HCI) and protocol notes.

Build system: CMake + Nix flake. Version in `VERSION` (currently `0.2.0-dev`).

License: keep existing (check headers; prefer GPLv3+ / REUSE / SPDX for new files).

## Standing rules (from session prompt)

- Continuity: maintain `TODO.md` so any agent can continue from tip + repo alone.
- Deliverables: git bundles only (full stack from original base of the work line). No patches, no parallel histories.
- Author: `Ingo Ruhnke <grumbel@gmail.com>`
- Trailer on every commit: `Co-authored-by: Grok <grok@x.ai>`
- Prefer correct design; no quick hacks; call out refactors.
- Never remove features without explicit request.
- Sandbox: work in temp dirs; copy only finished bundles to artifacts.
- Testing: do not spend long on full builds/tests if the user can do them faster; hand over the bundle.

## Architecture quick map

| Path | Role |
|------|------|
| `lib/include/s226/protocol.hpp` | Pure packet builders / decoders |
| `lib/include/s226/watch.hpp` | High-level Watch API (scan, connect, bind, HR/BP/steps, `send`) |
| `lib/include/s226/usb.hpp` | USB BT controller discovery |
| `lib/include/s226/step_rate.hpp` | Cadence from step counter |
| `lib/src/` | HCI host, USB transport, protocol, watch, step_rate |
| `app/watch_bridge.*` | Qt adapter: signals + non-blocking `request` / `requestStream` |
| `app/main_window.*` | Tab shell, connection toolbar, Live tab |
| `app/settings_tab.*` | Settings form (Refresh / Apply) |
| `app/alarms_tab.*` | Alarm list + weekday picker dialog |
| `app/notify_tab.*` | Messages, call, music, MPRIS keys |
| `app/mpris_controller.*` | Optional MPRIS media-key bridge |
| `app/notification_forwarder.*` | Desktop notification → watch |
| `app/history_tab.*` | 5-minute activity slots |
| `app/sleep_tab.*` | Sleep sessions (0xE0) |
| `app/workouts_tab.*` | Sport-mode sessions |
| `cli/` | CLI (`Session` + Actions; same protocol helpers) |
| `data/`, `man/`, `udev/` | Desktop integration, man pages, udev rule |
| `PROTOCOL.md`, `VeePoo_HBand_BLE_Protocol.md` | Reverse-engineering notes |
| `s226.py`, `s226_bumble.py` | Research / probe tools |

## GUI request pattern

`Watch` exposes `send()` and `rawNotification`. Structured query/set is
done in the GUI via `WatchBridge::request` / `requestStream` (accept
predicate + timeout, results on the GUI thread). The CLI has its own
`Session`/`Inbox` for the same idea. Prefer reusing `s226::protocol`
builders/decoders; do not duplicate wire formats.

## Work-line base (this sequence)

- Base commit: `94f9363` (Add VeePoo_HBand_BLE_Protocol.md)
- Bundle naming: `s226-NNN.M-slug-94f9363.bundle`
- Keep only the current tip bundle in artifacts.
