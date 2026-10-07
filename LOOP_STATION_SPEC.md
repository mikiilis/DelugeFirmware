# Loop Station Algorithm Specification

## Overview
Eight-track live looper inspired by Boss RC-505. Each track operates independently with its own phrase, history, and state. Designed for live performance with immediate, responsive gestures.

Updated 2026-09-08 to reflect the implemented menus and unified Start Action / Stop Action settings and continuous overdub.
For setup, all user-visible configuration, controls, and examples, see the [Loop Station user guide](website/src/content/docs/features/loop_station.mdx).
Performance guarantees below are acceptance requirements, not a claim that hardware acceptance testing is complete.

## Feature Flag & Enablement
- **Runtime setting**: `EnableLoopStation` (community runtime, default: Off)
- **Location**: Settings → Community Features → Enable Loop Station
- **Build**: Compile with feature enabled; toggle at runtime via menu

## Component Architecture

- `src/deluge/loop_station/loop_station.h/.cpp`: firmware-independent settings, gesture/state engine, timing, geometry, routing helpers and recorder admission.
- `LoopStationView`: gestures, LEDs, menus, capture delegation and asynchronous revision publication.
- `AudioEngine::tickSongFinalizeWindows`: services action deadlines and splits audio windows at fractional musical positions. Graphics never executes queued starts/stops.
- `AudioClip`: native-speed playback and sample-frame phase for bound loops, including song load before opening this view.
- `SampleRecorder`: ordinary initial take capture; bounded cluster-cache writer for continuous cumulative overdub.
- `Song`: settings, bindings, immutable WAV revision history and migration. The unused parallel `loop_station::core` implementation has been removed.

Timing uses Q16 musical positions for pending actions and sample frames for recorded audio. Display and storage work do not own the action clock.

## Dependency Rule
`Song/AudioClip → SongModel → Engine ← LoopStationView`
Pad scanning, display calls, file serialization, and `SampleRecorder` ownership must never move into `Engine`.

## Build & Verify
```sh
# Run host unit tests
./dbt test

# Build firmware
./dbt build debug
```

---

## Core Concepts

### Tracks
- 8 independent tracks (slots 1-8)
- Each track maintains its own phrase, phrase length, playback position, and revision history
- Tracks can be in different states simultaneously

### Backing Audio Clips
- Each Loop Station slot is backed by a normal, visible Audio Clip in Song View
- Backing clips are created lazily when a slot is first selected, configured, or recorded
- A newly created backing clip receives an independent Audio Output / FX chain and the default name `LOOP 1` through `LOOP 8`
- Clearing a phrase releases its revisions but retains the backing clip, output, name, and track settings
- Deleting a backing clip from Song View unbinds its Loop Station slot and releases that slot's revision history

### Phrase

The initial recording establishes a positive sample-frame length. All later playback and cumulative revisions retain that length. Musical tick length remains clip metadata; tempo changes never stretch or trim an existing loop.

An overdub starts at the current sample position immediately, repeats continuously, and includes additions from previous passes. Stopping overdub immediately closes its input gate. If stopped before its first full pass, background work copies only untouched backing frames, even after playback or transport stops. One completed overdub session creates one undo revision, regardless of its number of passes.

## Live Performance Guarantees

**Performance and live experience is the highest priority.** The system must guarantee:

- **Zero gaps / silence / glitches** during recording, overdubbing, or playback
- **Seamless loop transitions** — phrase boundaries are imperceptible
- **Responsive gesture feedback** with no unintended musical wait: recording and playback use Start Action and Stop Action. Stopped taps dispatch on release to distinguish hold-to-clear. Overdub input always starts and stops immediately.
- **Existing loop remains audible** during overdub (no dropout, no mute)
- **Input monitoring audible** in real-time during capture states
- **Background commit** — UI never blocks waiting for SD write; state transitions feel instant
- **No audio artifacts** at state boundaries (RECORDING→PLAYING, PLAYING⇄OVERDUBBING, STOPPED→PLAYING)

These guarantees apply to all 8 tracks simultaneously.

### Recorder Capacity
- The shared Audio Engine admits at most **16 retained recorder objects**, including setup reservations, active captures, sealed writers, and recorder-backed playback providers. Durable history metadata does not count.
- A separate **16 channel-unit** limit covers active capture. Mono physical first takes cost 1 unit; stereo first takes and every overdub cost 2, even when the configured physical input is mono.
- Loop Station channel units are released when actual input capture ends, including initial-take latency-compensation frames. Its retained-object reservation remains until the recorder object is released. Ordinary recorder users keep their existing lifetime channel accounting.
- Both limits are checked before recorder setup can yield. A full limit rejects the new take as a resource error, preserves the preceding audio, and is never reported as an SD-card failure.
- Each Loop Station initial take is limited to **8 unwritten clusters**, including the current cluster. A cluster remains charged while its write is in flight and becomes available again only after a successful write. Reaching the limit rejects the take without publishing partial audio.
- Hardware and SD media must still support eight simultaneous stereo Loop Station captures; the object and cluster bounds do not guarantee SD throughput or audio continuity.

---

### Revisions (History)
- Each completed recording or overdub creates a new cumulative revision
- Revisions are immutable WAV files containing all layers up to that point
- Unlimited undo/redo depth (bounded only by storage)
- Each revision knows the phrase length and the logical phase of its sample zero

---

## User Interaction Model

### Gestures

| Gesture | Description |
|---------|-------------|
| Single press (button) | Primary action for current state |
| Double press (within 350ms, button) | Secondary action (stop/abort) |
| Hold 2s on STOPPED (button) | Clear phrase |
| Single press (border) | Select/deselect track |
| SHIFT + press (button) | Show track settings on OLED |
| Cross-screen edit button (Song View) | Open Loop Station (requires EnableLoopStation=On) |

**Entry Point:**
- **Cross-screen edit button** while the root UI is Song View (`SessionView`)
- Opens Loop Station view (4×2 pad grid)
- Requires **EnableLoopStation = On** in Settings → Community Features
- If disabled: shows popup "Enable in Community Features"
- Loop Station cannot be opened from Arranger View; the Arranger's existing cross-screen edit behavior remains unchanged
- If Song View is visible while Arrangement playback or recording is active, show an unavailable popup and do not open Loop Station, stop the Arrangement, or switch playback modes

**Track Selection (border press):**
- Press border → selects track (highlighted border, targets that track's FX and monitoring)
- Press border again → deselects track
- **When selected:**
  - FX panel applies to this track
  - Input monitoring audible (if enabled for this track)

**SELECT knob** always opens **global Loop Station settings** (Record Action, Start Action, Stop Action)

**SHIFT + Button** (press any LED in track's button):
- Opens that track's settings directly on OLED
- Navigate with SELECT knob, BACK to exit

### State Transitions

| Current state | Gesture | Result |
| --- | --- | --- |
| Empty | Centre press | Record at Start Action deadline |
| Recording | Centre press | Finish at Stop Action deadline, immediately enter Play or Dub according to Record Action |
| Playing, Loop | Centre press | Begin continuous overdub immediately |
| Playing, One Shot | Centre press | Restart from logical beginning at Start Action deadline |
| Overdubbing | Centre press | Close input immediately, return to Play; finish untouched backing in background |
| Playing / Overdubbing | Double tap | Discard provisional double-tap overdub; stop backing at Stop Action deadline |
| Stopped | Short centre release | Restart from logical beginning at Start Action deadline |
| Stopped | Hold two seconds, release | Clear phrase/history, retain settings |
| Pending user action | Centre tap | Cancel the pending action; automatic record-duration limit remains |
| Error | Centre press | Acknowledge only; next gesture performs an action |

A stopped hold fills the border clockwise in red over two seconds. Releasing earlier performs the normal stopped tap. SHIFT-centre opens settings and does not animate deletion.

Visible performance state is independent of WAV durability. A completed initial capture begins playback or automatic overdub at the capture boundary, without a second Start Action wait or an SD-finalization wait. History navigation and clear remain guarded while a working revision is incomplete.

### Start Action and Stop Action

These are the only timing controls and are song-wide. Each offers **Immediate** or **Synced**. Synced reveals an Interval selector with 1 bar, 1/2 bar, 1/4 bar, 1/8 bar, 1/16 bar, 1/32 bar and a triplet variant of each.

Defaults: **Start = Synced, 1/16 bar**; **Stop = Synced, 1/16 bar** (both straight).

Immediate executes when the gesture dispatches. Synced counts strictly future tempo pulses, not global bar lines:

| Interval | Future pulses |
| --- | --- |
| 1 bar | Four quarter-note metronome clicks |
| 1/2 bar | Two quarter-note clicks |
| 1/4 bar | Next quarter-note click |
| 1/8 bar | Next half-click subdivision |
| 1/16 bar | Next quarter-click subdivision |
| 1/32 bar | Next eighth-click subdivision |
| Triplet | Same pulse count with pulse spacing multiplied by 2/3 |

A gesture exactly on a pulse waits for the following pulse. The clock uses the metronome offset and continues to define timing when the audible metronome is disabled. Fractional subdivisions retain sub-tick precision; audio windows are split at their deadline, rounded up to the next audio sample. Tempo changes affect pending actions but never recorded sample lengths or speed.

Example: start 1/4, stop 1 bar. Press Record: blink red until the next click, then record. Press again: keep recording through four future clicks, finish on the fourth and enter Play/Dub immediately. Double-tap a playing loop: blink the pending stop while audio continues through four future clicks, then stop.

### Group actions and transport

Start All queues eligible stopped loops at a common Start Action deadline. It never restarts already playing tracks. Stop All cancels queued starts and covers recording, overdubbing and playback: initial takes finish at Stop Action and stay stopped; overdub input closes immediately while backing playback uses Stop Action. Each track retains independent phrase length.

Transport shutdown cancels queued actions, finishes initial takes immediately, closes overdub gates and stops playback. Untouched audio and durable commits drain in background. Leaving the view follows Stop Action for initial recording, closes overdub input immediately, cancels queued starts, and waits for safe publication.

### Audio and phase

Initial recordings retain every captured frame; there is no Smart crop or tempo snap. Fixed Record Duration is a separate automatic limit in bars; cancelling a manual pending stop does not cancel that limit.

Bound loops play at native recorded speed before and after tempo changes, including restored songs. Stopped starts and one-shot retriggers begin at logical phrase zero. Ordinary Audio Clip FX remain available.

The first take owns logical phase zero. A cumulative WAV may have physical frame zero at the overdub start position. Store that offset as `sampleZeroFrames`, modulo phrase frames. Translate logical and physical positions using this offset; never recalculate phase from the changed song tempo. The progress ring uses the actual playback sample position. A one-shot ends when audio playback ends, not when the old musical tick length expires.

Overdub captures dry backing plus physical input with saturation, before transpose, FX, level and pan. Silence retains earlier layers. The writer pins a bounded eight-cluster cache and one SD snapshot; dirty or in-flight entries cannot be evicted. Each pass reads its prior cumulative data. SD callbacks may yield to audio without losing newer changes. No allocation or SD write occurs in the per-frame cumulative mixer. The pending-input FIFO is released once sealed input drains, source ancestry and source readers are released when the working generation is fully materialized, and the write snapshot is released after the WAV closes successfully. A recorder-backed playback reader is handed off after the ordinary clip voice is ready at the matching frame.

On overdub input stop, no subsequent input is captured. If untouched audio remains, bounded background copying uses the retained source sample. Source ancestry and readers can be released as soon as the complete working generation is materialized; the previous durable revision remains available for rollback until publication succeeds or the working generation is rejected.

## Undo / Redo

### Structure
- Each track: ordered revision list + cursor (-1 = empty state)
- Cursor points to current revision
- Revision = complete cumulative WAV + metadata

### Operations
- **Undo**: cursor--, load revision (or empty if cursor < 0)
- **Redo**: cursor++, load revision
- **New Overdub After Undo**: truncate redo branch, append new revision, cursor++
- **Clear (hold STOPPED)**: release all revisions, cursor = -1

### Persistence
- All revisions retained in song (RAM metadata + SD WAVs)
- Song save moves temp WAVs to permanent sample folder
- Load restores revision list and cursor position

---

## Monitoring

### Physical Input Monitoring (Audible)
| State | Behavior |
|-------|----------|
| RECORDING | Always audible if enabled |
| PLAYING | Audible if track selected and no other track capturing same source |
| OVERDUBBING | Audible for overdubbing track if Monitoring is enabled; recording input is independent of this switch |
| FINISHING_OVERDUB | No capture input; only untouched backing is copied |
| Priority | Capturing tracks > Selected idle track |

### Input FX
- **No FX printed to WAV** — all audio recorded dry
- Track FX chain applied live during playback/monitoring
- Input FX (if any) part of track FX chain, not baked in

### Input Sources
Physical only: LEFT, RIGHT, STEREO, BALANCED

---

## Error Handling

### Capture Errors (Affect Single Slot)
- Resource allocation fail → ERROR, preserve and continue playing the previous phrase
- A genuine recorder write/finalization error → SD CARD ERROR, preserve and continue playing the previous revision
- Full card, missing card, missing filesystem, and write-protected card retain their specific error instead of collapsing to generic SD CARD ERROR
- Maximum file size → MAX FILE SIZE ERROR, preserve and continue playing the previous revision
- Backing missing or incomplete (voice unavailable during overdub) → BACKING UNAVAILABLE, reject the take and keep the previous revision
- Missing physical input → SOURCE UNAVAILABLE, reject the take and keep the previous revision
- Non-SD capture failures must never be presented as SD card errors

### Async Commit Errors
- Performance state can be Play, Dub or Stopped while a WAV is pending durability.
- The 16-object retained-recorder limit includes sealed writers and recorder-backed playback. Loop Station capture-channel units are released when input capture ends; ordinary recorder channel accounting remains charged through recorder destruction.
- A working revision can be audible before publication; the prior durable revision is retained for rollback.
- History is appended and redo truncated only after successful finalization.
- Failure rejects the working revision, preserves prior durable audio and latches an error on that slot.
- A dependent overdub using the rejected revision closes input immediately and is aborted before rollback. The slot rejects conflicting gestures until rollback and error notification finish; an already requested stop remains stopped after error acknowledgement.
- Aborted recorders release writer resources once but retain their Sample ownership and retained-object reservation until dependent, playback and history references drain. Temporary-sample cleanup must respect these references; it must not delete or unlink audio still in use.
- Cache underruns report backing unavailable, independently of filesystem errors. Dirty overdub cache entries receive round-robin write service.
- Ending overdub prefetches for audible playback and background copying independently. Background copying waits for missing/loading destination buffers without advancing capture; it must not report a temporary destination shortage as missing backing.
- Partial-sector overdub writes must reach storage before raw cluster readers can reload them. Successful short seeks/writes are failures, never accepted audio.
- Storage diagnostics retain the first filesystem operation/result and byte counts through rollback. Active capture failure is guarded against reentrant gestures and immediately closes input and detaches the rejected output recorder.
- Monitoring is recomputed on failure and acknowledgement using the existing capture/selected-track priority; an unselected idle track does not acquire a monitoring return solely because its capture failed.
- Successful publication does not restart playback or move its phase.

### Error Acknowledgement
- ERROR is a latched, fast red-blinking state; unrelated tracks remain operational
- The next centre-button press acknowledges the error and restores `stateBeforeError` (or the prior stable state)
- The acknowledgement press performs no other primary action; starting, stopping, recording, or overdubbing requires a subsequent press

---

## Edge Cases

- Mid-phrase overdub and subsequent passes preserve the original sample-frame length and logical beginning.
- Very short loops can wrap multiple times in one audio buffer; each visit includes the preceding addition.
- A stop during first-pass overdub closes input immediately; backing remainder copying continues with transport stopped.
- A pending manual stop can be cancelled without deleting a fixed recording-duration limit.
- A failed allocation, source read or SD write affects one slot; it must not publish incomplete audio or truncate redo.
- Undo to empty then record creates a replacement branch only after successful publication.
- Loading duplicate slot IDs retains the first valid binding; later clips remain ordinary unbound Audio Clips.

## Configuration (Per Track)

| Parameter | Range | Default |
|-----------|-------|---------|
| Source | LEFT, RIGHT, STEREO, BALANCED | LEFT |
| Monitoring | ON/OFF | ON |
| Record Duration (bars) | Off = manual; 1 to min(65535, maximum sequence ticks / current bar length) | Off |
| Playback | LOOP, ONE_SHOT | LOOP |
| Transpose | -24.50 to +24.50 semitones, 0.01 steps; stored as semitones and cents | 0.00 |
| Progress Colour | 11 colours: Gold, Yellow, Lime, Turquoise, Cyan, Sky, Blue, Purple, Magenta, Pink, White | Per-track default; exact Red, Orange, and Green remain reserved for track states |
| Copy To Track | Any of the other seven slots; SELECT confirms | No copy until confirmed |

Progress colour defaults for tracks 1–8 are Yellow, Cyan, Blue, Purple, Magenta, White, Gold, and Lime.
The menu also exposes the backing Audio Clip's Actions, Master, Audio Compressor, Filters, FX, Sidechain, Sample, Attack, and Priority controls. These retain their ordinary Audio Clip semantics and are not copied by Copy To Track.
Source Track and Dim Border Colour exist internally but are not exposed in the current Loop Station menus.

---

## Configuration (Global)

| Parameter | Choices | Default |
| --- | --- | --- |
| Record Action | REC > PLAY > DUB; REC > DUB > PLAY | REC > PLAY > DUB |
| Start Action / Mode | Immediate; Synced | Synced |
| Start Action / Interval | 1, 1/2, 1/4, 1/8, 1/16, 1/32 bar; triplet variants | 1/16 bar |
| Stop Action / Mode | Immediate; Synced | Synced |
| Stop Action / Interval | 1, 1/2, 1/4, 1/8, 1/16, 1/32 bar; triplet variants | 1/16 bar |

### Settings Persistence and Migration

The device-wide enable switch remains in `SETTINGS/CommunityFeatures.XML`. Song XML stores `startActionMode`, `startActionInterval`, `stopActionMode`, and `stopActionInterval` inside `loopStation`. Mode is 0 = Immediate, 1 = Synced. Interval IDs remain 0–3 = bar/half/quarter/eighth and 4–7 = their triplet variants; 8 = 1/16 bar, 9 = 1/32 bar, 10 = 1/16 bar triplet, and 11 = 1/32 bar triplet. These saved IDs are independent of menu order (six straight intervals followed by six triplets). Existing valid action settings, including Immediate mode, are preserved. Missing or invalid modes use Synced; missing or invalid intervals use straight 1/16 bar.

Ignore removed legacy `quantize`, `playbackTiming`, `startTiming`, `stopTiming`, `startFromStopped`, `loopSync`, `startMode`, `stopMode`, and fade fields. They do not override new defaults. Preserve Record Action, audio, bindings, other track settings and revision history. New saves omit obsolete timing fields. Legacy tick-based sample-zero offsets are converted against the saved phrase/sample lengths when frame offsets are absent.

## Transpose Algorithm

### Overview
Real-time pitch shifting applied to **live monitoring** and **playback** — never baked into WAVs. Audio is recorded dry; transpose is a live effect.

### Parameter Model
- **Semitones**: -24 .. +24 (coarse transpose)
- **Cents**: -50 .. +50 (fine tune)
- **Total range**: ±24.5 semitones
- **Combined value**: `totalCents = semitones * 100 + cents`

### Conversion to Pitch Ratio
```
pitchRatio = 2^(totalCents / 1200)
phaseIncrement = round(pitchRatio * kMaxSampleValue)
```
- `kMaxSampleValue = 1 << kBitDepth` is the firmware-native Q24 unity value
- `phaseIncrement = kMaxSampleValue` = unity pitch
- `phaseIncrement > kMaxSampleValue` = pitch up
- `phaseIncrement < kMaxSampleValue` = pitch down

### Implementation Architecture

**LivePitchShifter** (granular/time-domain algorithm):
- Double-buffered play heads with crossfade (prevents clicks)
- Maintains circular repitched buffer per track
- Interpolation buffer for fractional sample reads
- Hop-based processing: read ahead, crossfade between play heads

**Application Point**:
- Each track has one combined pre-FX pitch-shifter stage
- The stage processes the audible dry loop plus any audible live monitor together, using one `LivePitchShifter` instance and one pitch ratio
- A playing phrase therefore remains transposed even when the track's Monitoring setting is OFF
- Any live input made audible by the monitoring policy is transposed through the same stage
- Recorder taps branch before this stage, so initial recordings and overdubs always capture dry, untransposed audio

### State Management
- **Pitch shifter allocated** during Loop Station audio-output setup and retained until that output is destroyed; allocation does not depend on monitoring or transpose
- **Repitching enabled** when: transpose or cents produce a non-unity phase increment; unity is an exact bypass
- **Shifter state reset** when repitching resumes after a bypass, or the input sample clock has a discontinuity
- **Transpose and cents edits** update the phase increment without reallocating or resetting an actively repitching shifter
- **Crossfade progress** preserved across buffer callbacks for continuity

### Interpolation
- Linear interpolation between adjacent samples in repitched buffer
- `interpolationBufferSize` determined by `pitchRatio` (larger for extreme shifts)
- Max interpolation buffer: 64 samples

### Performance Constraints
- Runs on audio thread (DSP context), no allocations
- Fixed buffers: repitched buffer (8192 samples), interpolation buffers
- CPU scales with `|pitchRatio - 1.0|` — extreme shifts cost more
- Mono/stereo handled: stereo = two channels processed in parallel

### Invariants
1. Transpose never written to WAV (audio always recorded dry)
2. Monitoring and playback use identical pitch ratio
3. Pitch shifter allocation is independent of transpose; entering repitch after bypass resets its state
4. Zero transpose = bypass shifter entirely (unity pass-through)

### Signal Chain Position
**Transpose is PRE-FX** — pitch shifting applied before track FX chain:

```
Clip playback (dry WAV) + audible live input (dry)
        │
        ▼
LivePitchShifter (transpose applied here)
        │
        ▼
Saturation → Filters → FX (delay, reverb, etc.) → Volume → Compressor → Output
```

The combined audible signal is transposed once and then flows through all track FX in real-time. Dry recorder taps are taken before the combined pitch-shifter stage.

---

### LED Grid: 16×8 (16 cols × 8 rows)

Each **pad** = 4×4 LED square. Grid = **4 pads wide × 2 pads high** = 8 pads = 8 tracks.

```
PAD GRID (4×2 = 8 pads)
┌────┬────┬────┬────┐
│ T1 │ T2 │ T3 │ T4 │  ← Physical top row (LED rows 4-7)
│    │    │    │    │
├────┼────┼────┼────┤
│ T5 │ T6 │ T7 │ T8 │  ← Physical bottom row (LED rows 0-3)
│    │    │    │    │
└────┴────┴────┴────┘
```

Track numbering is row-major from the physical top-left: `1, 2, 3, 4` across the top, then `5, 6, 7, 8` across the bottom. The backing clips use the corresponding default names `LOOP 1` through `LOOP 8`.

### Track Anatomy (per pad = 4×4 LED square)

```
TRACK = one 4×4 pad = 4×4 LED square
┌─────────────────────┐
│  4×4 LED SQUARE     │  ← Track = button + border
│  ┌───────────────┐  │
│  │ ┌─────────┐   │  │  ← Border (1px) = progress ring
│  │ │  2×2    │   │  │
│  │ │ button  │   │  │  ← Button (2×2) = press target
│  │ └─────────┘   │  │
│  └───────────────┘  │
└─────────────────────┘
```

**Terminology:**
- **Button** = inner 2×2 LED region (gesture target)
- **Border** = 1-pixel LED ring around button (progress ring)
- **Track** = button + border (one 4×4 pad)

### OLED Display

**Root OLED display**: `Loop Station`. Track selection is indicated on the pad border; menu navigation opens the corresponding settings screen.

**Menu Modes** (toggled by SELECT knob press):

| Mode | Trigger | Content |
|------|---------|---------|
| Global Menu | SELECT press | Record Action, Start Action, Stop Action |
| Track Menu | SHIFT + Track button | Source, Monitoring, Record Duration, Progress Colour, Playback, Transpose, Copy To Track, backing Audio Clip controls |

**Navigation**: SELECT rotate = scroll, SELECT press = enter/toggle, BACK = exit

### SHIFT + Button (Track Settings)

**SHIFT + press** any LED in track's button (inner 2×2):
- Opens that track's settings directly on OLED
- Navigate with SELECT knob, BACK to exit

### Copy Track Configuration

- In a track's settings, choose **Copy To Track**, select one of the other seven tracks, then press SELECT to confirm
- The open track is the source and the chosen track is the destination
- Copies source, source-track reference, monitoring, duration, progress colour, playback mode, loop sync, start mode, stop mode, fade time, and transpose
- The destination's existing backing-clip identity, recorded audio, name, revision/undo history, playback state, Audio Output / FX chain, and global settings remain unchanged
- An unbound destination may receive its normal empty `LOOP n` backing clip so the copied routing can be applied; this does not create or copy audio or revisions
- The copied input routing, colour, and monitoring behavior apply immediately
- A destination that is RECORDING, OVERDUBBING, finishing an overdub, or COMMITTING is busy; the copy is rejected without changing any setting

### Colour Legend (Button + Border)

| State | Button | Border |
|-------|--------|--------|
| EMPTY | Gray | Dim song colour |
| RECORDING | Red solid | Red pulse |
| OVERDUBBING | Orange solid | Orange pulse |
| PLAYING | Green solid | Track-colour clockwise progress |
| STOPPED | Dim green | Dim track colour; 2-second clockwise red fill while held for deletion |
| COMMITTING | Orange blink | Orange blink |
| ERROR | Red blink fast | Dim red |

- **Button** = main state colour (inner 2×2)
- **Border** = progress ring clockwise from the physical top-left; animated for PLAYING progress and STOPPED delete confirmation

### Global Hardware LEDs

| LED | Behaviour |
|-----|-----------|
| **REC** (red) | Any track capturing (RECORDING, OVERDUBBING, FINISHING) |
| **BACK** (amber) | Undo/Redo available on selected track (not busy) |

### Right Sidebar Group Controls

The physical top row of the two-column right sidebar contains the Loop Station group controls:

| Sidebar pad | Colour | Action |
|-------------|--------|--------|
| Left | Green | **START ALL** stopped phrases |
| Right | Red | **STOP ALL** stable playing phrases |

- Enabled controls are bright; unavailable controls remain dim.
- Start All and Stop All use their respective shared action deadlines described above.
- Sidebar LEDs blink while a corresponding action is pending. Track LEDs show the pending destination until execution or cancellation.

### Gesture Mapping (Button Press)

| Gesture | Action |
|---------|--------|
| Single press | Primary action for current state |
| Double press (within 350ms) | Secondary action (stop/abort) |
| Hold 2s on STOPPED | Clear phrase (→ EMPTY) |

The STOPPED hold displays a non-wrapping clockwise red border sweep. All 12 border LEDs are red at 2 seconds and remain red until the centre button is released to delete.

---

## Invariants (Must Always Hold)

1. An accepted phrase has positive frame length; tempo changes never change it.
2. Overdub is continuous and additive; its input gate closes immediately on stop.
3. One overdub session publishes one revision, preserving logical sample-frame zero.
4. Action deadlines are shared by individual and sidebar gestures; graphics never executes them.
5. A pending gesture is cancellable independently of the automatic recording limit.
6. A working WAV cannot replace durable history until finalization and close succeed.
7. Dirty/in-flight recorder cache entries retain ownership through yielding SD operations.
8. Clear, history and copy preserve clip identity, routing rules and other slots' state.
9. Physical pad numbering remains top-left to bottom-right; all eight tracks are independent.
10. Recorder admission is globally bounded by 16 retained objects, 16 active capture channel units, and 8 unwritten clusters per Loop Station initial take.

## Validation

Host coverage includes state transitions, future-click scheduling, straight/triplet intervals, fractional boundaries, pending cancellation, group controls, continuous overdub, input gates, revision publication, resource accounting, routing, geometry and error recovery. Recorder tests exercise production capture lifecycle, cumulative short-loop writes, early-stop preservation, yielding writes and failure cleanup.

Run the five host suites (UnitTests, RecorderLifecycleTests, RecorderFinalizationTests, LoopStorageTests, and LoopRevisionPublicationTests) with UBSan, build Debug firmware, and build the website. Hardware acceptance remains required for eight simultaneous stereo tracks on supported slow SD media, long phrases across cache windows, rapid dependent overdubs, both Record Actions, tempo changes, monitoring off, transpose extremes, transport stop, Save/Load during capture, and saved/reloaded undo history. Check audio continuity and sample-frame phase on OLED and seven-segment models, and record queue/buffer high-water marks, write latency, allocations, and audible gaps.

### Capture transaction lifetime

A slot remains busy throughout capture preflight, recorder setup, finishing,
publication and rollback, including callbacks which yield to audio/SD/UI work.
Gestures cannot replace or clear that slot during these operations. Other slots
remain operational. Exit waits for all transactions; exit or transport stop
requested during startup must drain the newly attached recorder. Reopening the
view must not discard pending commits. Load/clear during capture first drains
an exit to Song View, where Load can be pressed again.

Repeated overdubs preserve cumulative audio and durable undo history across
publication, rollback and vector growth. Existing loop tempo and immediate
overdub input start/stop behavior remain unchanged.
