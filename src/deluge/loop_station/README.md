# Loop Station architecture

This directory contains the platform-independent control plane for the experimental eight-track Loop Station. The firmware view and song serializer depend on it, while the host unit-test target compiles it without the hardware audio engine.

The feature is deliberately gated by the `EnableLoopStation` runtime community setting. Its default is Off.

## Responsibilities

`SongModel` owns persistent global and per-slot configuration. `Song` owns each slot's persistent WAV revision list and cursor. `Engine` owns runtime state, independent scheduled events, capture lifecycle calls, and revision navigation. `LoopStationView` owns pad gestures and rendering and implements the capture delegate by using each backing `AudioClip`'s `SampleRecorder`. `gui/menu_item/loop_station/settings.cpp` adapts the model's typed parameters to the standard Sound Editor menu framework. `FanOutRouter` and `RecorderBudget` define audio-routing and recorder-allocation invariants without depending on the hardware renderer.

The intended dependency direction is:

```text
Song / AudioClip persistence
          |
          v
      SongModel <--- LoopStationView
          |               |
          v               v
        Engine ----> EngineDelegate ----> recorder integration
          |
          +----> FanOutRouter / RecorderBudget
```

Do not put pad scanning, display calls, file serialization, or `SampleRecorder` ownership in `Engine`. This keeps its scheduling behavior deterministic and host-testable.

## Runtime model

There are exactly eight `TrackRuntime` entries, indexed by the strongly typed `SlotId`. Each entry owns its pending event, so unrelated tracks may schedule and finish independently.

An event is expressed in song ticks. `processTick()` executes every due event and schedules fixed-duration completion once the current bar length is known. `ticksUntilNextEvent()` lets Session playback include Loop Station work in its next-event calculation.

The state transition implemented by a centre gesture is:

```text
EMPTY -> RECORDING -> OVERDUBBING -> PLAYING  (REC > DUB > PLAY)
EMPTY -> RECORDING -> PLAYING -> OVERDUBBING  (REC > PLAY > DUB)
PLAYING -> OVERDUBBING -> PLAYING             (immediate gestures)
PLAYING -> STOPPED                            (double tap)
STOPPED -> PLAYING                            (centre)
STOPPED -> EMPTY                              (two-second hold in the view)
```

Record and overdub transitions happen on the first centre-pad down event, rather than waiting for release of the
coalesced 2x2 gesture. A stopped phrase remains release-dispatched so the view can distinguish Play from a two-second
Clear hold. `SHIFT + centre` remains a menu gesture and never triggers the performance action.

The first completed recording establishes `phraseLength`. A manual overdub starts immediately at the current phrase
position and captures exactly one complete pass while preserving that length and ignoring the fresh-take fixed-duration
setting. Pressing centre during the pass returns the visible state to `PLAYING` immediately; monitored input stops and
the recorder fills the untouched remainder with the old loop before committing the new cumulative WAV. One-shot tracks
retrigger from `PLAYING` and transition to `STOPPED` after `phraseLength` ticks. They never start an overdub.

The first take also permanently establishes the phrase's logical phase-zero. Because a cumulative WAV captured by a
mid-loop overdub physically begins at that mid-loop phase, the backing Audio Clip persists the logical phase represented
by sample-zero. Playback translates between those phases instead of rebasing `playbackStartTick`, so every layer remains
at the performed position and the original start and length remain unchanged.

`COMMITTING` prevents another gesture until `notifyCommitComplete()` arrives. `ERROR` is a visual failure latch. Capture and commit implementations must fail only the affected slot and preserve the previously completed phrase.

## Timing rules

- Off: use the performed tick.
- Smart: start immediately; snap a stop to the nearest bar boundary when it lies within an eighth note.
- Measure: queue start or stop at the next bar boundary. A gesture already on a boundary starts there; a stop deliberately targets the following boundary.
- Fixed duration (fresh recordings only): `captureStartTick + durationBars * barLength`, clamped to
  `kMaxLoopSequenceLength`.

The view derives beat pulses and border progress from the high-resolution musical position, including the fractional
position within the current tick. Pad-matrix Y increases upward, so the physical top-left coordinate is `(0, 3)` inside
each cell. Playing progress starts there, advances clockwise, and
crossfades each leading pad from the dim border colour to the track colour. Recording and overdub use a continuous
triangle brightness pulse whose peak lands on each song beat. Never introduce a wall-clock animation phase for musical
state.

Border pads toggle the selected monitoring track without invoking the centre action. Selecting an unused slot creates
its normal backing Audio Clip so an enabled physical monitor becomes audible immediately. The selected cell receives a
white brightness lift across its whole border except while Recording or Overdubbing, where the red/orange state pulse
must remain unmodified. Selection is optional, and pressing the selected border again clears it. Centre
gestures—including `SHIFT + centre` for settings—must not alter selection or monitoring ownership.
Selection also makes that Audio Clip the active modulation target, so the normal quick-FX buttons and gold encoders edit
the selected loop. With no selection they target the song. Standard View button dispatch remains active; `SHIFT + TAP`
toggles the metronome.

## Capture delegate contract

The `EngineDelegate` call order for a successful synchronous take is:

1. `preflight(slot, overdub)`
2. `beginCapture(slot, overdub, tick)`
3. `finishCapture(slot, overdub, length, crop)`
4. `stateChanged(slot, destination)`

If `finishCapture()` reports an asynchronous operation, the engine enters `COMMITTING`. The delegate must eventually call `notifyCommitComplete()`. `requestExit()` is not ready until every capture and commit has completed.

A delegate must treat an overdub as a copy-on-write operation. `abortCapture(slot, true)` discards a provisional layer
without modifying the prior phrase. One completed pass appends one immutable revision while retaining the established
phrase length. Each revision stores the Audio Clip file path, sample bounds, musical length, and logical sample-zero.
With a loop selected by its border, BACK moves to the previous revision and SHIFT + BACK moves to the next revision.
For an initial take the previous revision is the empty slot, so undo removes the phrase and redo restores it. A failed
file reload leaves the current phrase and history cursor unchanged. A successful capture after undo discards the redo
branch only after the new cumulative WAV has committed.

The firmware delegate starts and finishes the normal linear recorder on the slot's ordinary Audio Clip. It preserves
the configured track name, derives the endpoint from the live musical position rather than the last scheduler event,
and reconciles the sample endpoint with the performed or Smart-snapped tick length. This keeps playback at the recorded
speed while permitting the small trim or extension requested by Smart quantization. It restores the prior length when
an overdub is aborted and keeps global Recording mode active while any slot captures. Overdub completion always reuses
the original Audio Clip length. Recorder allocation, SD, and maximum-size failures are reported back to the affected
engine slot. Overdub recorders tap the stored loop plus monitored input before track FX, level, pan, compression, and
normalization. Both components are restored to unity at the recorder, so the old material is copied unchanged and the
new input is added only across the interval for which Overdub was visibly active. The Monitoring setting controls only
the audible live return; physical input continues to be recorded when Monitoring is Off.

Starting an individually stopped phrase while the Session transport is stopped starts the internal playback clock
before resuming its Audio Clip. This ordering is required by `AudioClip::resumePlayback()` and prevents the `E430`
no-active-clock assertion.

## Audio invariants

- Input FX process only incoming material and are printed once.
- Pitch, reverse, track FX, level, and pan are playback operations and must not enter stored samples.
- Every overdub copies the old sample at unity and sums new input with signed 32-bit saturation.
- There are no Replace or Feedback modes: silence during overdub preserves the previous revision sample-for-sample.
- A shared tap source renders once. Physical monitor returns deduplicate only when source and input-FX fingerprint both match.
- Bound Player-mode Audio Outputs use a runtime-only monitoring override for physical inputs. It is active only while
  the Loop Station view owns monitoring and is never serialized as the legacy Audio Output mode. Capturing tracks have
  priority, followed by the selected idle track; muted outputs do not claim a de-duplicated return.
- Overdub capture enables that monitoring override as part of the state transition, before the output recorder can
  render its first buffer. The bounce therefore contains the previous loop plus the new physical input layer.
- The backing voice is positioned before the output recorder attaches and is rendered into every cumulative buffer,
  even when normal clip-activity routing would suppress it. If any buffer cannot render the backing phrase, the
  provisional overdub is rejected and the previous WAV is retained rather than committing input-only or silent data.
- Loop Station outputs must be excluded from Loop Station mix sources, and a slot must not record its own Audio Output.
- Progress colours exclude green, red, and orange because those colours are reserved for Play, Record, and Overdub.
- The direct `FX` entry in each track menu edits the normal Audio Output effect suite for that slot, so effects remain
  independently configurable and serialized per loop track.
- The normal Audio Clip controls are flattened into the track menu. `FX` is shared with that set and appears only once;
  there is no redundant `AUDIO CLIP` wrapper submenu.

## Persistence

`Song::writeToFile()` writes the `loopStation` song object, including each slot's ordered revision list and current
cursor. `Song::readFromFile()` supplies defaults for missing fields and calls `SongModel::sanitize()` for untrusted enum
and range values. Legacy songs seed a one-entry history from the bound Audio Clip.

Every historical sample remains retained by the song, so normal song saving moves any temporary WAV used by current,
undo, or redo state into the song's sample folder. Clearing a phrase releases the whole revision list. History is
bounded by available RAM for metadata and SD-card space for WAV files rather than by a fixed revision count.

The backing `AudioClip` writes `loopStationSlot="1"` through `"8"`. Zero is unbound. On load, the first valid clip for a slot wins; duplicate IDs are cleared. Generic clip cloning intentionally leaves the ID at zero. Removing the bound Session clip clears the song binding and releases that slot's revision history.

Settings not represented by `TrackSettings` continue to use normal Audio Clip and Audio Output serialization.

## Tests and builds

The host coverage is in `tests/unit/loop_station_tests.cpp` and is registered in `tests/unit/CMakeLists.txt`.

```sh
./dbt test
./dbt build debug
```

The firmware build produces `build/Debug/deluge.elf`, `.bin`, and `.hex`.

## Integration status

The control plane, view, persistence, feature gating, geometry, menus, physical monitoring, and recording are connected.
Captured takes become ordinary WAV files and play through the slot's Audio Output FX. Loop Station overdubs use a
dedicated pre-FX dry tap: the old loop and monitored input are merged with saturating addition while track FX remain
outside the cumulative file. Persistent multi-level undo and redo navigate immutable WAV revisions per slot. Input-FX
printing and internal-source fan-out still need dedicated hardware-path coverage.
