# TODO / handoff

## Current tip

- Repo tip: `94f9363` (Add VeePoo_HBand_BLE_Protocol.md)
- Work-line base (for bundle ranges): `94f9363`
- Branch: master
- Version: 0.2.0-dev
- No prior agent bundles in this sequence.

## Open work / known gaps (from README / PROTOCOL)

Protocol / reverse engineering still open:

- Meaning of the post-bind `0xA7` dump (`0xAD` and `0xB8` are decoded)
- `0xD1` history bytes 14-16 and 18; `0xD3` reply layout; workout header bytes 33-38
- Role of the `fee7` and HID services
- Trailing status byte in 0xD0 / 0x90 frames
- Message display time fixed at ~5 s
- Alarm (`b1` / `b9`) and countdown exact layouts
- Notification (ANCS-style) path
- `f002` secondary channel role (raw PPG samples while measuring)
- Settings not fully implemented: brightness schedule, alarms, countdown, screen style (`c7`)

Code / product:

- Distance/calories intentionally omitted from GUI (watch estimates).
- Python tools are research-only; production path is C++ + raw HCI.

## Session notes

- Initial checkout: shallow then unshallowed; 52 commits on master.
- Created `AGENTS.md` and this `TODO.md` for continuity (they were missing).
- No functional changes yet; waiting for a concrete task.

## Next actions

(None assigned yet — provide a task.)
