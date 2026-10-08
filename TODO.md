# TODO / handoff

## Current tip

- Work-line base: `94f9363`
- Latest bundle: artifacts `s226-011…`

## Done in this sequence

1. WatchBridge `request` / `requestStream` / `send` / `connectedChanged`
2. Tabbed s226-hr: Live, Settings, Alarms, Notify, History, Workouts
3. Man page, README, AppStream for the tabs
4. Alarm weekday checkboxes (All / Clear; empty days → one-shot)
5. AGENTS.md architecture map updated; `#include <algorithm>` in settings_tab

## User-side

- Build and smoke-test on hardware (`nix run .#s226-hr`)

## Protocol gaps (unchanged)

- 0xA7 dump; D1 bytes 14-16/18; fee7/HID role
