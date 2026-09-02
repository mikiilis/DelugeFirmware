# Loop Station Algorithm Specification

## Overview
Eight-track live looper inspired by Boss RC-505. Each track operates independently with its own phrase, history, and state. Designed for live performance with immediate, responsive gestures.

## Feature Flag & Enablement
- **Runtime setting**: `EnableLoopStation` (community runtime, default: Off)
- **Location**: Settings → Community Features → Enable Loop Station
- **Build**: Compile with feature enabled; toggle at runtime via menu

## Component Architecture

### Core Layers

**Engine Layer** (pure logic, no I/O)
- Track runtime: 8 independent slots with tick scheduling, state machine, revision navigation
- Audio routing: Fan-out router for physical input capture (LEFT, RIGHT, STEREO, BALANCED)
- Resource management: Recorder budget allocation, buffer management
- Invariants: All timing in song ticks; no display, file, or hardware dependencies

**View Layer** (UI & gestures)
- Pad gesture handling: press, double-press, hold with configurable thresholds
- Visual feedback: centre pad colors, border progress, status LEDs
- Engine delegate: bridges UI events to engine commands, manages recorder lifecycle

**Settings/Menu Layer**
- Per-track configuration: source, monitoring, duration, progress colour, playback mode, sync, fades, transpose, and copy-to-track
- Global feature gate: runtime enable/disable setting

**Model/Persistence Layer**
- Song model: per-slot revision lists with cursors (save/load, sanitize, migration)
- Audio clip binding: links tracks to clip storage, provides recorder hooks
- Sample recorder: low-level capture/encoding used by view delegate

**Audio Processing Layer**
- Pre-FX dry tap: capture point before track FX for overdub layering
- Monitoring overrides: per-state audible input routing with priority rules

**Playback Integration Layer**
- Tick source: scheduler ticks (swung) for all position calculations
- Next-event hooks: transport integration for auto-stop, quantization

**Localization Layer**
- Menu strings, seven-segment display text

**Test Layer**
- Host unit tests: state machine, revisions, phase math, quantization, routing, budget

**Documentation Layer**
- User-facing docs, architecture invariants (deep doc)

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
- A complete loop cycle with fixed musical length (in ticks)
- Established by the first recording on an empty track
- Length never changes after first recording
- **Overdub behavior:**
  - Press during PLAYING → overdub starts **immediately** at press tick (no quantization)
  - Overdub **always runs exactly one full phrase length** (overdubEndTick = overdubStartTick + phraseLength)
  - Cannot stop overdub early — it captures exactly one loop cycle
  - Press again during overdub → UI returns to PLAYING immediately; recorder finishes phrase in background
  - Double-tap during overdub → aborts, discards the overdub layer
  - Result: new cumulative WAV (all previous layers + new input), same phrase length, same playback start tick

---

## Live Performance Guarantees

**Performance and live experience is the highest priority.** The system must guarantee:

- **Zero gaps / silence / glitches** during recording, overdubbing, or playback
- **Seamless loop transitions** — phrase boundaries are imperceptible
- **Immediate response** to all gestures (press, double-tap, hold) — no perceptible latency
- **Existing loop remains audible** during overdub (no dropout, no mute)
- **Input monitoring audible** in real-time during capture states
- **Background commit** — UI never blocks waiting for SD write; state transitions feel instant
- **No audio artifacts** at state boundaries (RECORDING→PLAYING, PLAYING⇄OVERDUBBING, STOPPED→PLAYING)

These guarantees apply to all 8 tracks simultaneously.

### Recorder Capacity
- Loop Station capture capacity is **16 channel units**: mono capture costs 1 unit and stereo capture costs 2 units
- Supported hardware and SD media must accept eight simultaneous stereo Loop Station captures
- Capacity is enforced by the shared Audio Engine recorder pool before a slot enters a capture state
- Recorders remain charged while capturing and while their WAV is still being written, finalized, or closed; only a recorder handed back for deletion releases its units
- Cost follows the actual WAV format: every overdub and internal source is stereo, even when its configured input is mono
- Non-Loop-Station recorder users participate in the same budget, so rapid gestures cannot overcommit the writer pool
- When capacity is occupied by finalization, a new capture is rejected as a resource limit and must never be reported as an SD-card failure
- Recorder insertion is FIFO: new captures join behind existing writers and cannot repeatedly starve older recorders

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
| Double press (< 350ms, button) | Secondary action (stop/abort) |
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
- Press border → selects track (highlighted, shows track name in OLED title)
- Press border again → deselects track
- **When selected:**
  - FX panel applies to this track
  - Input monitoring audible (if enabled for this track)

**SELECT knob** always opens **global Loop Station settings** (Record Action Mode, Record Quantize, Start From Stopped)

**SHIFT + Button** (press any LED in track's button):
- Opens that track's settings directly on OLED
- Navigate with SELECT knob, BACK to exit

### State Transitions

```
EMPTY ──press──► RECORDING ──press──► PLAYING ⇄ OVERDUBBING
  ▲                                      │           │
  │                                      ▼           ▼
  └─────────────── hold 2s ◄──── STOPPED ◄───────────┘
       ▲                         │
       │         press           │ double-tap
       └─────────────────────────┘

PLAYING ⇄ OVERDUBBING = infinite cycle (press toggles, double-tap → STOPPED)
```

**From EMPTY:**
- Press → RECORDING (quantized start)

**From RECORDING:**
- Press → PLAYING (quantized stop, phrase length locked)
- Auto-stop (fixed duration) → PLAYING
- Auto-stop (RECORD_OVERDUB_PLAY mode) → COMMITTING, then OVERDUBBING from the commit-completion position

**From PLAYING:**
- Press → OVERDUBBING (immediate, no quantization)
- Double-tap → STOPPED
- One-shot auto-stop → STOPPED after phrase length

**From OVERDUBBING:**
- Press → PLAYING (immediate UI, recorder finishes phrase in background)
- Double-tap → STOPPED (abort, discard overdub layer)
- Auto-commit at phrase end → PLAYING

**From STOPPED:**
- Release (short press) → PLAYING (per **Start From Stopped** global setting: RESUME_QUANTIZED or FROM_START)
- Hold 2s → EMPTY (clear all revisions, keep settings)
- During the hold, the track border fills red clockwise from the physical top-left over exactly 2 seconds
- At the threshold the complete red ring remains lit; releasing confirms deletion, while releasing earlier performs the normal short-press action
- SHIFT-held centre gestures do not show the delete animation and continue to open Track Settings

**From COMMITTING:**
- Wait for async write → return to PLAYING or STOPPED (stateBeforeCommit)

### Playback Start/Stop Modes
- Start Mode and Stop Mode apply only to playback of an existing phrase
- They do not fade or delay RECORDING→PLAYING, PLAYING→OVERDUBBING, or OVERDUBBING→PLAYING transitions; those transitions remain gapless and immediate
- **Start Mode = IMMEDIATE**: a stopped phrase becomes audible immediately at the start phase selected by **Start From Stopped**
- **Start Mode = FADE**: a stopped phrase begins at the selected start phase and fades from silence to its configured level over **Fade Time**
- **Stop Mode = LOOP_END**: playback continues until the next phrase boundary, then stops
- **Stop Mode = IMMEDIATE**: playback stops immediately using a short anti-click ramp at the audio boundary
- **Stop Mode = FADE**: playback fades to silence over **Fade Time**, then enters STOPPED
- **Fade Time** is measured in musical bars at the current song tempo

---

## Record Action Modes (Global Setting)

| Mode | Behavior |
|------|----------|
| RECORD_PLAY_OVERDUB (default) | Record → Play → Overdub → Play → Overdub... |
| RECORD_OVERDUB_PLAY | Record → durable commit → Overdub (auto from commit-completion position) → Play → Overdub... |

---

## Quantization Rules

### Recording Start/Stop
- **OFF**: Immediate at press tick
- **SMART**: Start immediate; stop snaps to nearest bar if within 1/8 note
- **MEASURE**: Start at next bar; stop at next bar after press

### Overdub
- **Always immediate** - no quantization for overdub start
- **Always runs exactly one full phrase length** from press tick
- **Cannot be quantized** - performance gesture

---

## Audio Behavior

### First Recording (Initial Take)
- Captures selected **physical input** (LEFT, RIGHT, STEREO, BALANCED) **dry** (no FX, no normalization)
- Uses fixed unity capture gain; automatic normalization and post-record file alteration are disabled for every Loop Station WAV
- Establishes phrase length and playback start tick
- Sets logical phase zero = phrase start (sample 0 = loop start)

### Playback
- With **Loop Sync = ON**, playback preserves the phrase's fixed tick length and song phase; existing audio is time-stretched when tempo changes
- With **Loop Sync = OFF**, playback runs at its recorded speed and free-runs independently of later tempo changes
- Sample-rate conversion may still be applied when the recording and system sample rates differ; it is separate from musical time-stretching
- **Track FX chain applied in real-time** to combined audio
- With Loop Sync ON, playback phase is derived from: `(currentTick - playbackStartTick) % phraseLength`
- With Loop Sync OFF, playback position advances in the sample domain and the current phase is saved when stopped
- The engine playback gain and persistent AudioOutput playback gain are synchronized at every playback, history, clear, restore, and capture boundary
- STOPPED is the only stable state that retains zero playback gain; EMPTY and a fresh RECORDING use unity as their latent baseline
- Undo-to-empty, hold-to-clear, and undo-to-revision restore unity before the next take or selected revision becomes audible

### Overdub (Additive Layer)
**Trigger:** Press during PLAYING
**Start:** Immediate at press tick (captures overdub start tick)
**Duration:** Exactly one phrase length (overdubEndTick = overdubStartTick + phraseLength)
**Capture:**
- Existing loop playback (from WAV, dry) at unity gain
- Physical input (dry, pre-FX, pre-level, pre-pan) at unity gain
- The backing render and physical input enter the cumulative mixer at the same internal one-eighth scale
- They are added first, then the combined signal receives one saturating ×8 compensation back to stored-WAV unity
- Saturation is a signed 32-bit clamp after combined gain restoration; arithmetic must never wrap
- No feedback attenuation is applied: a silence overdub preserves every previous layer at the same level across unlimited revisions, within 24-bit storage rounding
**User Experience:**
- Existing loop remains audible during overdub
- Input adds on top in real time
- Silence during overdub preserves previous layer perfectly
**Completion:**
- Press centre → UI returns to PLAYING immediately
- Recorder continues in background until phrase end
- On completion: commits new cumulative WAV (dry, all layers combined)
- Phrase length and playback start tick **never change**

### Overdub Chain (Infinite)
```
Record → Play → Overdub → Play → Overdub → Play → Overdub → ...
```
- Each overdub creates a new cumulative revision
- Unlimited overdubs (storage permitting)
- Each revision contains all previous layers + new input
- Undo steps back through revisions one at a time
- Redo steps forward (branch discarded on new overdub after undo)

### One-Shot Playback
- Retriggers from playbackStartTick on press
- Stops automatically after phraseLength ticks
- Never enters overdub mode

---

## Phase Model (Critical for Mid-Phrase Overdubs)

### Definitions
- **Playback Start Tick**: Absolute tick where phrase logically begins (set on first recording)
- **Phrase Length**: Fixed musical length in ticks (set on first recording)
- **Overdub Start Tick**: Absolute tick where overdub recording begins (can be mid-phrase)
- **Sample Zero Phase**: Logical phrase position represented by physical sample 0 in WAV

### Phase Calculations
```
Logical Phase at Tick = (Tick - PlaybackStartTick) % PhraseLength

Physical Sample Position =
    LogicalPhase >= SampleZeroPhase
        ? LogicalPhase - SampleZeroPhase
        : PhraseLength - (SampleZeroPhase - LogicalPhase)
```

### Initial Recording
- SampleZeroPhase = 0 (physical sample 0 = phrase start)
- Playback position = Logical Phase

### Overdub Recording
- SampleZeroPhase = LogicalPhaseAt(OverdubStartTick)
- Physical sample 0 in new WAV = overdub start position
- During overdub: voice positioned at PhysicalSamplePosition

### Post-Commit Playback
- Initial recording: voice starts at position 0
- After overdub: voice positioned at PhysicalSamplePosition for current logical phase

---

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
| OVERDUBBING | Always audible for overdubbing track |
| FINISHING_OVERDUB | **Not audible** (recorder still captures input but user hears loop only) |
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
- COMMITTING state blocks new gestures on that track
- The recorder and its channel units remain owned by the slot until its WAV has been finalized and closed
- A provisional revision is not appended to undo/redo history until finalization succeeds
- During overdub commit, the previous durable revision remains audible; the provisional cumulative WAV is swapped in only after success
- On fail: preserve the prior revision, latch ERROR, and remember `stateBeforeError`
- On success: complete state transition

### Error Acknowledgement
- ERROR is a latched, fast red-blinking state; unrelated tracks remain operational
- The next centre-button press acknowledges the error and restores `stateBeforeError` (or the prior stable state)
- The acknowledgement press performs no other primary action; starting, stopping, recording, or overdubbing requires a subsequent press

---

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| Overdub at phrase boundary (tick = playbackStartTick + n×phraseLength) | SampleZeroPhase = 0, behaves like initial recording |
| Rapid double-press during overdub (< 350ms) | Treated as double-tap → STOPPED, overdub aborted |
| Overdub runs full phrase without press | Auto-commits at phrase end, returns to PLAYING |
| Smart stop on recording | Snaps to bar, phraseLength = snapped length |
| Transport stopped during capture | Wait for commit, then exit view |
| Switch song with active loops | Loops stop, revisions preserved |
| Undo to empty, then new recording | Redo branch discarded, new revision appended |
| Load song with loops | Revisions loaded, cursor restored, clips bound |
| Overdub immediately after record (RECORD_OVERDUB_PLAY) | Auto-starts after the first WAV commits, from that current musical position, and records one complete phrase |
| One-shot retrigger during play | Resets to phrase start, plays once |
| Multiple tracks recording simultaneously | Each independent, shared sources render once, shared writer capacity includes all active and finalizing recorders |

---

## Configuration (Per Track)

| Parameter | Range | Default |
|-----------|-------|---------|
| Source | LEFT, RIGHT, STEREO, BALANCED | LEFT |
| Monitoring | ON/OFF | ON |
| Duration (bars) | 0=Manual, 1-65535 | Manual |
| Playback | LOOP, ONE_SHOT | LOOP |
| Loop Sync | ON/OFF | ON |
| Start Mode | IMMEDIATE, FADE | IMMEDIATE |
| Stop Mode | LOOP_END, IMMEDIATE, FADE | LOOP_END |
| Fade Time (bars) | 1-65535 | 1 |
| Transpose | -24..+24 semitones | 0 |
| Transpose Cents | -50..+50 | 0 |
| Progress Colour | 11 colours: Gold, Yellow, Lime, Turquoise, Cyan, Sky, Blue, Purple, Magenta, Pink, White | Per-track default; exact Red, Orange, and Green remain reserved for track states |

---

## Configuration (Global)

| Parameter | Range | Default |
|-----------|-------|---------|
| Record Action Mode | RECORD_PLAY_OVERDUB, RECORD_OVERDUB_PLAY | RECORD_PLAY_OVERDUB |
| Record Quantize | OFF, SMART, MEASURE | SMART |
| Start From Stopped | RESUME_QUANTIZED, FROM_START | RESUME_QUANTIZED |

- **Record Quantize** controls only the initial recording start/stop rules defined above; overdub remains immediate
- **Start From Stopped = RESUME_QUANTIZED**, **Loop Sync = ON**: start on the next bar at the song-aligned phrase phase
- **Start From Stopped = RESUME_QUANTIZED**, **Loop Sync = OFF**: resume immediately from the phrase phase saved when the track stopped
- **Start From Stopped = FROM_START**: begin from phrase phase zero without a quantized wait, regardless of Loop Sync; Start Mode still controls the gain ramp

---

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
- **Pitch shifter created** when: (phrase playback is audible OR live monitoring is audible) AND transpose ≠ 0
- **Pitch shifter reset/recreated** when: transpose or cents change (avoids artifacts)
- **Pitch shifter destroyed** when: transpose = 0, or neither phrase playback nor live monitoring is audible
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
3. Pitch shifter state reset on transpose change (no glitches)
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

**Title Bar**: `Loop Station    [Track 3]    ▼`

**Menu Modes** (toggled by SELECT knob press):

| Mode | Trigger | Content |
|------|---------|---------|
| Global Menu | SELECT press | Record Action Mode, Record Quantize, Start From Stopped |
| Track Menu | SHIFT + Track button | Source, Monitoring, FX, Duration, Playback, Loop Sync, Start/Stop Mode, Fade Time, Transpose, Progress Colour, Copy To Track |

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
- The copied input routing, Loop Sync setting, colour, and monitoring behavior apply immediately
- A destination that is RECORDING, OVERDUBBING, finishing an overdub, or COMMITTING is busy; the copy is rejected without changing any setting

### Colour Legend (Button + Border)

| State | Button | Border |
|-------|--------|--------|
| EMPTY | Dim/off | Off |
| RECORDING | Red solid | Red pulse |
| OVERDUBBING | Orange solid | Orange pulse |
| PLAYING | Green solid | Green clockwise progress |
| STOPPED | Dim green | Dim green static; 2-second clockwise red fill while held for deletion |
| COMMITTING | Orange blink | Orange blink |
| ERROR | Red blink fast | Red blink fast |

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
- With Record Quantize set to MEASURE, every affected track uses the same next measure boundary. Other quantize modes act immediately.
- START ALL preserves each track's configured Start From Stopped, playback, phase-resume, and fade behaviour.
- STOP ALL stops stable playback as a group and does not truncate RECORDING, OVERDUBBING, background overdub completion, or COMMITTING tracks.
- EMPTY and ERROR tracks are ignored by both controls.

### Gesture Mapping (Button Press)

| Gesture | Action |
|---------|--------|
| Single press | Primary action for current state |
| Double press (< 350ms) | Secondary action (stop/abort) |
| Hold 2s on STOPPED | Clear phrase (→ EMPTY) |

The STOPPED hold displays a non-wrapping clockwise red border sweep. All 12 border LEDs are red at 2 seconds and remain red until the centre button is released to delete.

---

## Invariants (Must Always Hold)

1. Phrase length > 0 when any revision exists
2. Playback start tick set on first recording, never changes
3. Sample zero phase ∈ [0, phraseLength)
4. Overdub end tick = overdub start tick + phraseLength when overdubbing
5. FinishingOverdub → recorder still running, UI in PLAYING
6. Current revision matches history cursor + 1 (or 0 if empty)
7. Recorder cleared on abort/error before returning to engine
8. Loop output never zeroed during overdub (must be audible)
9. All positions use scheduler tick (lastSwungTickActioned)
10. Backing missing flag set if any overdub buffer missed backing render
11. Redo branch truncated on new overdub after undo
12. First takes and overdubs never enable normalization or post-record gain alteration
13. Dry backing and dry physical input enter the cumulative mixer at the same one-eighth scale
14. The combined cumulative mix is restored once with saturating ×8 gain; silence overdubs do not attenuate prior revisions
15. Delete progress is a non-wrapping 2-second clockwise border fill and never appears for SHIFT-held settings gestures
16. Engine and AudioOutput playback gain never diverge; clearing or undoing a stopped phrase restores a unity baseline before the next recording
17. Physical pad numbering is fixed top-left to bottom-right: tracks 1–4 on the top row and tracks 5–8 on the bottom row
18. Recorder capacity is shared globally and remains charged through asynchronous WAV finalization; each capture is charged for the channel count it actually records
19. A provisional take enters revision history only after its WAV is finalized and closed; failure restores the previous durable revision without truncating redo
20. Copy To Track replaces only the destination `TrackSettings`; it never copies or clears audio, clip identity, revision history, runtime state, FX, or global settings

---

## Test Coverage

- **Host unit tests**:
  - State machine transitions
  - Revision history (undo/redo, truncation on new overdub)
  - Phase calculations (mid-phrase overdub, boundary cases)
  - Quantization rules (OFF/SMART/MEASURE)
  - Start From Stopped × Loop Sync behavior
  - Start/stop mode and fade transitions
  - Latched error acknowledgement
  - FanOutRouter source mixing
  - Recorder admission, including actual mono/stereo costs, eight simultaneous stereo captures, and blocking rapid reuse while a prior commit is pending
  - Tempo-change behavior with Loop Sync on and off
  - Combined pre-FX transpose routing and dry recorder taps
  - Equal-gain backing/input capture, saturating overflow, and repeated silence overdubs with no cumulative level decay
  - Distinct backing-unavailable, source-unavailable, maximum-file-size, allocation, SD-full, missing-card, missing-filesystem, write-protected, and generic SD failure reporting
  - Asynchronous commit success/failure, transactional revision publication, and dependent overdub start after finalization
  - Clockwise delete-hold progress at start, midpoint, threshold, and beyond the threshold without wrapping
  - Stop → undo-to-empty → record, stop → hold-clear → record, and stopped undo-to-revision all restore audible unity gain
  - Physical pad-to-track mapping and default `LOOP 1`–`LOOP 8` naming from top-left to bottom-right
  - Track configuration copy, including every copied setting and preservation of destination clip binding and revision history

- **Run**: `./dbt test`
