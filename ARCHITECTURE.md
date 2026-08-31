# Loop Station — Architecture & Dev Context

Living document for the `feat/loopstation` work. Update it whenever a design
decision changes, a component moves, or the status/next-steps change — it
exists so a new session can get up to speed without re-deriving context.

Deep invariants (state transitions, timing rules, audio/persistence contracts)
live in [`src/deluge/loop_station/README.md`](src/deluge/loop_station/README.md).
This file is the operational overview: map, status, decisions, next steps.

## What it is

Eight-track live looping, gated by `EnableLoopStation` community runtime
setting (default Off). Each slot is backed by an ordinary `AudioClip` (Session
mode) whose `AudioOutput` FX suite is the track FX. Captures become WAV files;
overdubs are copy-on-write cumulative revisions; per-slot multi-level
undo/redo over immutable WAV revision lists.

## Repo / branch layout

- `origin` = `mikiilis/DelugeFirmware` (fork), `upstream` = `SynthstromAudible/DelugeFirmware`
- Working branch: `feat/loopstation`
- Feature commit to date: `db92c3eb` "Add Loop Station feature and documentation"

## Component map

```
src/deluge/loop_station/
  loop_station.h/.cpp      Engine: 8 TrackRuntime slots, tick scheduling, state
                           machine, revision navigation, FanOutRouter,
                           RecorderBudget. No I/O, no display, no file code.
  README.md                Full architecture invariants (the deep doc)

src/deluge/gui/views/
  loop_station_view.h/.cpp LoopStationView: pad gestures, borders/pulse
                           rendering, EngineDelegate implementation
                           (recorder lifecycle via AudioClip SampleRecorder)

src/deluge/gui/menu_item/
  loop_station/settings.cpp   track settings (TrackSettings) menu adapter
  runtime_feature/settings.cpp  EnableLoopStation gate

src/deluge/model/
  song/song.h/.cpp           Song: persistent per-slot revision lists + cursors
                             (save/load, sanitize, legacy seeding)
  clip/audio_clip.*          loopStationSlot binding, recorder hooks
  sample/sample_recorder.h   recorder support used by the view delegate

src/deluge/processing/audio_output.*  pre-FX dry tap, monitoring overrides
src/deluge/playback/(mode/session.cpp, playback_handler.h)  tick + next-event hooks
src/deluge/gui/l10n/*       strings (menu + seven_segment)

tests/unit/loop_station_tests.cpp     host tests (see Tests below)
website/src/content/docs/features/loop_station.mdx  user docs
```

Rule: dependency direction is Song/AudioClip -> SongModel -> Engine <-
LoopStationView. Pad scanning, display calls, file serialization, and
SampleRecorder ownership must never move into `Engine`.

## Status

- Connected: control plane, view, persistence, feature gate, menus, physical
  monitoring, recording, overdub, undo/redo.
- Open (needs dedicated hardware-path coverage):
  1. Input-FX printing path on hardware
  2. Internal-source fan-out on hardware

## Decision log

Append-only; newest last. (`date — decision — rationale`)

- 2026-08 — Overdub is append-only copy-on-write: one completed pass = one
  immutable cumulative revision; phrase length fixed by first take; no
  Replace/Feedback modes. Rationale: deterministic undo/redo and sample-safe
  restore on abort.
- 2026-08 — Logical phase: cumulative WAVs from mid-loop overdubs physically
  start mid-phrase; the clip persists sample-zero phase and playback
  translates phases instead of rebasing `playbackStartTick`.
- 2026-08 — Underscore naming / tick-based events: engine works purely in song
  ticks; the view alone deals with musical/visual position, so engine is
  host-testable without an audio engine.
- 2026-08 — Revision history bounded by RAM (metadata) + SD (WAV files), not
  a count; all historical WAVs are retained and moved into the song's sample
  folder on normal song save.
- 2026-08 — SLOT 0 = unbound; `loopStationSlot="1".."8"` persisted on AudioClip.
  Load: first valid clip per slot wins, duplicates cleared.

## Build & verify

```sh
./dbt test          # host unit tests incl. tests/unit/loop_station_tests.cpp
./dbt build debug   # firmware -> build/Debug/deluge.{elf,bin,hex}
```

## Next steps

- [ ] Hardware coverage: input-FX printing path
- [ ] Hardware coverage: internal-source fan-out
- [ ] (add items here as work lands)
