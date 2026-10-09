# TODO / handoff

## Current tip

- Work-line base: `94f9363`
- Latest bundle: artifacts `s226-55.1-fix-desktop-notify-94f9363.bundle`
- Tip: `f669f3d` fix desktop notification forwarder bugs

## Done in this sequence

### GUI
- Tabs: Live, Settings, Alarms, Notify, History, Sleep, Workouts
- Settings: weather status, features, message types, …
- Notify: messages, call, music, MPRIS forward/pull/auto-push, desktop
  notification forward (`org.freedesktop.Notifications`)
- System tray; single-instance (override `--multi-instance`)

### Library / CLI
- Sleep (`e0`), weather status (`c8`), contacts push (`72`), music (`99`)
- CLI: `--sleep`, `--weather`, `--contacts`, `--contact-delete`, `--music`, …

### Docs
- README, man s226-hr / s226-cli, AppStream, AGENTS, PROTOCOL notes

## User-side

- Hardware smoke-test: sleep data, weather toggle, contacts write, MPRIS,
  desktop notification forward (message type **other** enabled)

## Protocol gaps (research)

- Weather *forecast content* push (condition codes incomplete)
- SpO₂ history, GPS track download
- Classic sleep stage polarity / multi-nap edges
- Contact list *read* CRC

## Suggested next work

- Weather forecast content push (needs condition-code table or capture)
- Contacts GUI if write is confirmed on hardware
