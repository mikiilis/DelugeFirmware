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
- Per-track configuration: source, monitoring, duration, playback mode, sync, fades, transpose
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

**Track Selection (border press):**
- Press border → selects track (highlighted, shows track name in OLED title)
- Press border again → deselects track
- **When selected:**
  - FX panel applies to this track
  - Input monitoring audible (if enabled for this track)

**SELECT knob** always opens **global Loop Station settings** (Record Action Mode, Global Sync, etc.)

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
- Auto-stop (RECORD_OVERDUB_PLAY mode) → OVERDUBBING at phrase boundary

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

**From COMMITTING:**
- Wait for async write → return to PLAYING or STOPPED (stateBeforeCommit)

---

## Record Action Modes (Global Setting)

| Mode | Behavior |
|------|----------|
| RECORD_PLAY_OVERDUB (default) | Record → Play → Overdub → Play → Overdub... |
| RECORD_OVERDUB_PLAY | Record → Overdub (auto at phrase boundary) → Play → Overdub... |

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
- Establishes phrase length and playback start tick
- Sets logical phase zero = phrase start (sample 0 = loop start)

### Playback
- Loops seamlessly at recorded speed
- No timestretch unless sample rate differs from system
- **Track FX chain applied in real-time** to combined audio
- Playback position derived from: `(currentTick - playbackStartTick) % phraseLength`

### Overdub (Additive Layer)
**Trigger:** Press during PLAYING
**Start:** Immediate at press tick (captures overdub start tick)
**Duration:** Exactly one phrase length (overdubEndTick = overdubStartTick + phraseLength)
**Capture:**
- Existing loop playback (from WAV, dry) at unity gain
- Physical input (dry, pre-FX, pre-level, pre-pan) at unity gain
- Merged with saturating addition (32-bit signed clamp)
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
- Resource allocation fail → ERROR, preserve previous phrase
- SD card error / max file size → abort, keep previous revision
- Backing missing (voice lost during overdub) → reject, keep previous

### Async Commit Errors
- COMMITTING state blocks new gestures on that track
- On fail: ERROR, restore stateBeforeCommit
- On success: complete state transition

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
| Overdub immediately after record (RECORD_OVERDUB_PLAY) | Auto-starts at next phrase boundary |
| One-shot retrigger during play | Resets to phrase start, plays once |
| Multiple tracks recording simultaneously | Each independent, shared sources render once |

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
| Progress Colour | 6 colours (excl R/G/O) | Per-track default |

---

## Configuration (Global)

| Parameter | Range | Default |
|-----------|-------|---------|
| Record Action Mode | RECORD_PLAY_OVERDUB, RECORD_OVERDUB_PLAY | RECORD_PLAY_OVERDUB |
| Start From Stopped | RESUME_QUANTIZED, FROM_START | RESUME_QUANTIZED |

- **Start From Stopped = RESUME_QUANTIZED**: Short press on STOPPED resumes from quantized position (RC-505 style)
- **Start From Stopped = FROM_START**: Short press on STOPPED restarts from phrase beginning

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
phaseIncrement = pitchRatio * MAX_SAMPLE_VALUE  // Q32 fixed-point
```
- `phaseIncrement = MAX_SAMPLE_VALUE` (65536) = unity pitch
- `phaseIncrement > MAX_SAMPLE_VALUE` = pitch up
- `phaseIncrement < MAX_SAMPLE_VALUE` = pitch down

### Implementation Architecture

**LivePitchShifter** (granular/time-domain algorithm):
- Double-buffered play heads with crossfade (prevents clicks)
- Maintains circular repitched buffer per track
- Interpolation buffer for fractional sample reads
- Hop-based processing: read ahead, crossfade between play heads

**Application Points** (both use same `pitchRatio`):
1. **Monitoring path** (live input → output): `addPhysicalInputWithPitchShift()`
   - Applied when Monitoring = ON and track is capturing (RECORDING, OVERDUBBING) or selected idle
   - Physical input pitch-shifted in real-time before mixing to output

2. **Playback path** (WAV → output): `renderClipWithPitchShift()`
   - Applied to dry loop WAV during PLAYING/OVERDUBBING/STOPPED
   - Uses same `LivePitchShifter` instance as monitoring (shared state)

### State Management
- **Pitch shifter created** when: Monitoring enabled AND transpose ≠ 0
- **Pitch shifter reset/recreated** when: transpose or cents change (avoids artifacts)
- **Pitch shifter destroyed** when: Monitoring disabled or transpose = 0
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
Clip playback (dry WAV) + Live input (pitch-shifted)
        │
        ▼
LivePitchShifter (transpose applied here)
        │
        ▼
Saturation → Filters → FX (delay, reverb, etc.) → Volume → Compressor → Output
```

Both monitoring and playback paths share the same pitch ratio; the transposed signal then flows through all track FX in real-time.

---

### LED Grid: 16×8 (16 cols × 8 rows)

Each **pad** = 4×4 LED square. Grid = **4 pads wide × 2 pads high** = 8 pads = 8 tracks.

```
PAD GRID (4×2 = 8 pads)
┌────┬────┬────┬────┐
│ T1 │ T2 │ T3 │ T4 │  ← Pad Row 0 (LED rows 0-3)
│    │    │    │    │
├────┼────┼────┼────┤
│ T5 │ T6 │ T7 │ T8 │  ← Pad Row 1 (LED rows 4-7)
│    │    │    │    │
└────┴────┴────┴────┘
```

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
| Global Menu | SELECT press | Record Action Mode, Global Sync, etc. |
| Track Menu | SHIFT + Track button | Source, Monitoring, FX, Duration, Playback, Loop Sync, Start/Stop Mode, Fade Time, Transpose, Progress Colour |

**Navigation**: SELECT rotate = scroll, SELECT press = enter/toggle, BACK = exit

### SHIFT + Button (Track Settings)

**SHIFT + press** any LED in track's button (inner 2×2):
- Opens that track's settings directly on OLED
- Navigate with SELECT knob, BACK to exit

### Colour Legend (Button + Border)

| State | Button | Border |
|-------|--------|--------|
| EMPTY | Dim/off | Off |
| RECORDING | Red solid | Red pulse |
| OVERDUBBING | Orange solid | Orange pulse |
| PLAYING | Green solid | Green clockwise progress |
| STOPPED | Dim green | Dim green static |
| COMMITTING | Orange blink | Orange blink |
| ERROR | Red blink fast | Red blink fast |

- **Button** = main state colour (inner 2×2)
- **Border** = progress ring (clockwise from top-left), only animated in PLAYING

### Global Hardware LEDs

| LED | Behaviour |
|-----|-----------|
| **REC** (red) | Any track capturing (RECORDING, OVERDUBBING, FINISHING) |
| **BACK** (amber) | Undo/Redo available on selected track (not busy) |

### Gesture Mapping (Button Press)

| Gesture | Action |
|---------|--------|
| Single press | Primary action for current state |
| Double press (< 350ms) | Secondary action (stop/abort) |
| Hold 2s on STOPPED | Clear phrase (→ EMPTY) |

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

---

## Test Coverage

- **Host unit tests**:
  - State machine transitions
  - Revision history (undo/redo, truncation on new overdub)
  - Phase calculations (mid-phrase overdub, boundary cases)
  - Quantization rules (OFF/SMART/MEASURE)
  - FanOutRouter source mixing
  - RecorderBudget allocation

- **Run**: `./dbt test`
