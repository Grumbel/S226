# S226 — Agent notes

## Project overview

Linux tools for the **S226 fitness watch** (H-Band / Veepoo), based on a
reverse-engineered Bluetooth LE protocol. No phone and no H-Band app are
needed.

- **`s226-hr`**: Qt GUI for live heart rate, steps, cadence, graph, metronome.
- **`s226-cli`**: Terminal tool for the same plus history, workouts, battery, settings, notifications, etc.
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
| `lib/include/s226/watch.hpp` | High-level Watch API (scan, connect, bind, HR/BP/steps…) |
| `lib/include/s226/usb.hpp` | USB BT controller discovery |
| `lib/include/s226/step_rate.hpp` | Cadence from step counter |
| `lib/src/` | HCI host, USB transport, protocol, watch, step_rate |
| `app/` | Qt 6 GUI |
| `cli/` | CLI |
| `data/`, `man/`, `udev/` | Desktop integration, man pages, udev rule |
| `PROTOCOL.md`, `VeePoo_HBand_BLE_Protocol.md` | Reverse-engineering notes |
| `s226.py`, `s226_bumble.py` | Research / probe tools |

## Current tip (start of this work line)

- Commit: `94f936334f107b7b22a219cbf89bd912c21c6c9e` (short `94f9363`)
- Message: Add VeePoo_HBand_BLE_Protocol.md
- Branch: master
- Base for bundles: this commit (first checkout of the sequence)

Next bundle numbering starts at `s226-001.…` (or `projectname-001.…` — use `s226`).
