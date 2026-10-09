# TODO / handoff

## Current tip

- Work-line base: `94f9363`
- Latest bundle: (pending) MPRIS after weather/contacts/sleep
- Tip: optional MPRIS media-key bridge

## Done in this sequence

### GUI feature parity with CLI
- Tabs: Live, Settings, Alarms, Notify, History, Sleep, Workouts
- Settings: weather status on/off among other settings
- System tray, music now-playing, media keys
- Optional MPRIS: forward watch keys to system player; pull track into Notify form

### Library / CLI
- Sleep, weather status, contacts push
- CLI `--sleep`, `--weather`, `--contacts`, `--contact-delete`

### Docs
- PROTOCOL, man, README, AGENTS for the above

## User-side

- Hardware smoke-test: sleep, weather, contacts write, MPRIS with a playing app

## Protocol gaps (research)

- Weather forecast content push (condition codes)
- SpO₂ history, GPS track download
- Classic sleep polarity / multi-nap edges
- Contact list read CRC

## Suggested next work

- Weather forecast content push (needs condition-code table or capture)
- Contacts GUI if write is confirmed on hardware
- Auto-push MPRIS metadata to the watch on track change
