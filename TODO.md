# TODO / handoff

## Current tip

- Work-line base: `94f9363`
- Latest bundle: artifacts `s226-49.1-fix-notify-build-94f9363.bundle`
- Tip: `50f7888` fix notify tooltip + socket notifier build

## Done in this sequence

### GUI
- Tabs + tray + music + MPRIS + sleep + weather settings
- Desktop notification forward (session-bus eavesdrop → message type other)

### Library / CLI
- Sleep, weather status, contacts, music

## User-side

- Hardware smoke-test including desktop notify forward
  (needs session-bus eavesdrop; ensure message type "other" is enabled)

## Protocol gaps

- Weather forecast content, SpO₂ history, GPS, contact read CRC
