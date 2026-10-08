# TODO / handoff

## Current tip

- Work-line base: `94f9363`
- Latest bundle: artifacts `s226-016.1-music-docs-94f9363.bundle`
- Tip: document music in man/README (see `git log 94f9363..HEAD`)

## Done in this sequence

### GUI feature parity with CLI
- WatchBridge `request` / `requestStream` / `send` / `connectedChanged` / `musicControl`
- Tabs: Live, Settings, Alarms, Notify, History, Workouts
- Settings: sedentary, HR alarm, screen-on, brightness, countdown, watch face,
  person (write-only), features, message switches
- Alarms: list/add/edit/delete with weekday checkboxes
- Notify: messages, call ring/end, now-playing push, last media key
- History / workouts fetch
- System tray (hide on close, tooltip bpm, menu)
- Polish: auto-load Settings/Alarms on show, tables, tooltips, min size, BP gate

### Library / CLI
- Music: `nowPlayingPackets`, `decodeMusicControl`, `WatchEvents::musicControl`
- CLI `--music TITLE[/ARTIST[/ALBUM]][;playing=…][;vol=…]`
- Protocol unit tests for music encode/decode

### Docs
- AGENTS.md architecture map
- man s226-hr / s226-cli, README, AppStream for tabs + music + tray

## User-side

- Build and smoke-test on hardware (`nix run .#s226-hr`, `s226-cli --music …`)
- Confirm music feature toggle (`--feature music=on`) if metadata/keys ignored

## Protocol gaps (unchanged, research)

- 0xA7 dump meaning; D1 bytes 14-16/18; fee7/HID role
- Weather, contacts, SpO₂ history, GPS — present in Veepoo docs, not wired here

## Suggested next work (if desired)

- Weather push (if the face uses it)
- Contacts push
- Map watch media keys to local player (MPRIS) as an optional helper
