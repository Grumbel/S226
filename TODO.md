# TODO / handoff

## Current tip

- Work-line base: `94f9363`
- Latest bundle: artifacts `s226-44.1-fix-notify-mpris-94f9363.bundle`
- Tip: `dbbc46f` fix NotifyTab MprisController include/ctor

## Done in this sequence

### GUI
- Tabs: Live, Settings, Alarms, Notify, History, Sleep, Workouts
- Settings: weather status on/off
- System tray, music now-playing
- MPRIS: forward keys, pull track, auto-push track changes to watch

### Library / CLI
- Sleep, weather status, contacts push
- CLI `--sleep`, `--weather`, `--contacts`, `--contact-delete`

## User-side

- Hardware smoke-test: sleep, weather, contacts, MPRIS key forward + auto-push

## Protocol gaps (research)

- Weather forecast content push (condition codes)
- SpO₂ history, GPS track download
- Classic sleep polarity / multi-nap edges
- Contact list read CRC

## Suggested next work

- Weather forecast content push (needs condition-code table or capture)
- Contacts GUI if write is confirmed on hardware
