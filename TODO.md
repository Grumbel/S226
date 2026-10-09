# TODO / handoff

## Current tip

- Work-line base: `94f9363`
- Latest bundle: artifacts `s226-48.1-single-instance-94f9363.bundle`
- Tip: `4923f88` single-instance GUI ( --multi-instance override)

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
