# TODO / handoff

## Current tip

- Work-line base: `94f9363`
- See latest bundle in artifacts.

## Goal

Full feature set from s226-cli integrated into s226-hr via tabs.

### UI (done in this sequence)

Global toolbar: Controller, Watch, Connect, Log, Full screen.

Central `QTabWidget`:

1. **Live** — BPM view, trend graph, steps/cadence, metronome, BP
2. **Settings** — sedentary, HR alarm, screen-on, brightness, countdown, watch face, person, features, message switches; Refresh / Apply
3. **Alarms** — list / add / edit / delete
4. **Notify** — message + type, incoming call / end call
5. **History** — day selector, fetch 5-min slots table
6. **Workouts** — fetch sport-mode sessions with per-minute detail

### Bridge

WatchBridge: `request`, `requestStream`, `send`, `connectedChanged`, cancel on disconnect.

## Session notes

- Sandbox wipes `/tmp` frequently; work under `/home/workdir/s226`.
- Implementation complete; user should build/test with real hardware.
- Man page / README still need a short update for the new tabs (optional follow-up).

## Next actions

- Build and smoke-test on hardware.
- Optional: polish alarm day-of-week UI, README/man page.
