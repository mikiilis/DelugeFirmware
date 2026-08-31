# Loop Station Algorithm Specification

## Overview
Eight-track live looper inspired by Boss RC-505. Each track operates independently with its own phrase, history, and state. Designed for live performance with immediate, responsive gestures.

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
- All overdubs conform to this phrase length

### Revisions (History)
- Each completed recording or overdub creates a new cumulative revision
- Revisions are immutable WAV files containing all layers up to that point
- Unlimited undo/redo depth (bounded only by storage)
- Each revision knows the phrase length and the logical phase of its sample zero

---

## User Interaction Model

### Gestures
All gestures trigger on **centre pad press** (not release), except where noted.

| Gesture | Description |
|---------|-------------|
| Single press | Primary action for current state |
| Double press (< 350ms) | Secondary action (stop/abort) |
| Hold 2s on STOPPED | Clear phrase |

### State Transitions

```
EMPTY ──press──► RECORDING ──press──► PLAYING ──press──► OVERDUBBING ──press──► PLAYING
  ▲                                                                    │
  │                                                                    ▼
  └────────────────────── hold 2s ◄────────────── STOPPED ◄───────────┘
       ▲                                    │
       │              press (short)         │ double-tap
       └────────────────────────────────────┘
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

**From STOPPED:**
- Release (short press) → PLAYING (resume from quantized position)
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
- Captures selected source (physical inputs, mix bus, or other track)
- Establishes phrase length and playback start tick
- Normalization: ON for physical inputs, OFF for internal sources
- Sets logical phase zero = phrase start (sample 0 = loop start)

### Playback
- Loops seamlessly at recorded speed
- No timestretch unless sample rate differs from system
- Playback position derived from: `(currentTick - playbackStartTick) % phraseLength`

### Overdub (Additive Layer)
**Trigger:** Press during PLAYING
**Start:** Immediate at press tick (captures overdub start tick)
**Duration:** Exactly one phrase length (overdubEndTick = overdubStartTick + phraseLength)
**Capture:**
- Dry clip output (pre-FX, pre-level, pre-pan) at unity gain
- Physical input (if monitoring enabled) at unity gain
- Merged with saturating addition (32-bit signed clamp)
**User Experience:**
- Existing loop remains audible during overdub
- Input adds on top in real time
- Silence during overdub preserves previous layer perfectly
**Completion:**
- Press centre → UI returns to PLAYING immediately
- Recorder continues in background until phrase end
- On completion: commits new cumulative WAV
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
- Processed once during recording (printed to WAV)
- Bypassed during playback

### Input Sources
Physical: LEFT, RIGHT, STEREO, BALANCED
Internal: MIX_PRE_FX, MIX_POST_FX, SONG_TRACK (another track's output)

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
| Source | LEFT, RIGHT, STEREO, BALANCED, MIX_PRE_FX, MIX_POST_FX, SONG_TRACK | LEFT |
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

## UI Feedback

### Centre Pad Colours
- EMPTY: Dim
- RECORDING: Red pulse (beat-synced triangle)
- OVERDUBBING: Orange pulse
- PLAYING: Green
- STOPPED: Dim green
- COMMITTING: Orange blink
- ERROR: Red blink

### Border Progress
- Clockwise from top-left
- Green = played portion, dim = remaining
- Only visible in PLAYING state

### LEDs
- RECORD LED: Any track capturing (RECORDING, OVERDUBBING, FINISHING)
- BACK LED: Undo/Redo available (not busy)

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