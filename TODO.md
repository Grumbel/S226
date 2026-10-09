# TODO / handoff

## Current tip

- Work-line base: `94f9363`
- Latest bundle: (pending) sleep + weather/contacts
- Tip: weather status + contacts push after sleep tracking

## Done in this sequence

### GUI feature parity with CLI
- Tabs: Live, Settings, Alarms, Notify, History, Sleep, Workouts
- Settings includes weather status on/off
- System tray, music now-playing, media keys

### Library / CLI
- Sleep: `sleepRead`, `decodeSleepFrame`, `decodeSleepDay` (classic + V1)
- Weather status: `weatherStatusRead` / `Write` / `decodeWeatherStatus`
- Contacts: `contactWritePackets`, `contactDelete`, `contactMove`
- CLI `--sleep`, `--weather [on|off]`, `--contacts`, `--contact-delete`

### Docs
- PROTOCOL.md, man, README for sleep / weather / contacts

## User-side

- Hardware smoke-test: sleep with real data; weather on/off; contacts push
  (contacts read returned no reply on one S226 capture — write may still work)

## Protocol gaps (research)

- 0xA7 dump; D1 bytes 14-16/18; fee7/HID
- Weather *content* push (forecast TLV / condition codes) — framing only
- SpO₂ history, GPS track download
- Classic sleep stage polarity / multi-nap edges
- Contact list *read* CRC algorithm

## Suggested next work

- Weather forecast content push (needs condition-code table or capture)
- Optional MPRIS bridge for watch media keys
- Contacts GUI if write is confirmed on hardware
