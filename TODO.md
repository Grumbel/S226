# TODO / handoff

## Current tip

- Repo tip: `3819f36` (Finalize TODO.md for tip 081b511 and bundle 003)
- Work-line base (for bundle ranges): `94f9363`
- Branch: master
- Version: 0.2.0-dev
- Latest bundle: `/home/workdir/artifacts/s226-005.1-init-agents-todo-94f9363.bundle`
  (contains 94f9363..3819f36; supersedes prior)

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
- Commits in this sequence (all from base `94f9363`):
  - `3445c39` Add AGENTS.md and TODO.md for agent continuity
  - `96a0bda` Update TODO.md with current tip and first bundle
  - `081b511` Update TODO.md to tip 96a0bda and bundle 002
  - `3819f36` Finalize TODO.md for tip 081b511 and bundle 003
- Produced superseding bundle `s226-005.1-init-agents-todo-94f9363.bundle`.
- No functional changes yet; waiting for a concrete task.

## Next actions

(None assigned yet — provide a task.)
