# TODO / handoff

## Current tip

- Repo tip: `bbcdbcb` (Add tabbed GUI exposing full CLI feature set in s226-hr)
- Work-line base (for bundle ranges): `94f9363`
- Branch: master
- Version: 0.2.0-dev
- Latest bundle: `/home/workdir/artifacts/s226-007.1-tabbed-gui-full-features-94f9363.bundle`
  (range 94f9363..bbcdbcb)

## Goal (done)

Full feature set from s226-cli integrated into s226-hr via tabs.

### UI

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

- Implementation complete; user should build/test with real hardware.
- Optional follow-up: man page / README note about the tabs; richer alarm weekday picker.

## Next actions

- Build and smoke-test on hardware (`nix build` / `nix run .#s226-hr`).
