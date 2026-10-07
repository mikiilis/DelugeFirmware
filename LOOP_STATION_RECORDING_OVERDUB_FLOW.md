# Loop Station recording and overdub flow

This describes the implementation in this worktree. It separates musical timing,
audio capture, SD finalization, and revision publication because those operations
finish at different times.

Closing an overdub seals its input immediately and detaches capture from saving.
Playback retains that generation, and the next overdub can read its recorded
prefix plus its retained source for the untouched tail. It does not wait for a
cumulative WAV to finish. Up to 16 sealed generations per slot publish in order;
each take keeps its own undo revision without replacing newer live audio.

## Components

| Component | Responsibility |
| --- | --- |
| [`loop_station.cpp`](src/deluge/loop_station/loop_station.cpp) | Per-track state, gesture branches, Q16 deadlines, group actions, and commit bookkeeping |
| [`loop_station_view.cpp`](src/deluge/gui/views/loop_station_view.cpp) | Creates recorders, binds clips, stages revisions, reports errors, and resolves commits |
| [`audio_output.cpp`](src/deluge/processing/audio_output.cpp) | Renders playback, records physical input, and supplies audible monitoring |
| [`sample_recorder_loop.cpp`](src/deluge/model/sample/sample_recorder_loop.cpp) | Builds cumulative overdub WAVs with a fixed cache and retained source WAV |
| [`sample_recorder_card.cpp`](src/deluge/model/sample/sample_recorder_card.cpp) | Services file creation and SD writes outside the audio callback |
| [`revision.cpp`](src/deluge/loop_station/revision.cpp) | Owns cumulative WAV history and undo/redo |

Four milestones must not be confused:

1. **Input closed:** no later performer input can enter the take.
2. **Audio materialized:** every frame exists in the working cache or WAV.
3. **WAV complete:** dirty clusters, header, and file close succeeded on SD.
4. **Revision published:** the staged WAV entered undo history.

A track can show Playing or Stopped during milestones 2–4.
`committingLength != 0` identifies a pending durable commit. It does not block
playback stops or a supported dependent overdub.

## States and centre gestures

| Condition | Result |
| --- | --- |
| `finishingCapture` | Ignore the gesture for that track |
| `ERROR` | Acknowledge the error and restore Empty, Playing, or Stopped |
| Pending user action | Cancel it; an automatic duration deadline remains armed |
| `EMPTY` | Start or schedule the first recording using Start Action |
| `RECORDING` | Finish or schedule finish using Stop Action |
| `PLAYING`, loop | Start overdub immediately |
| `PLAYING`, one shot | Retrigger using Start Action |
| `PLAYING`, double tap | Stop playback using Stop Action |
| `OVERDUBBING`, normal tap | Close input immediately; finish the dub in background |
| `STOPPED`, short release | Restart from logical phrase zero using Start Action |
| `STOPPED`, two-second hold | Clear audio and history if no finish or commit is active |

Double taps use a 350 ms window and remember whether the preceding tap created
the current overdub:

- Playing → Overdubbing → second tap: abort that newly created dub, then stop.
- Established Overdubbing → first tap: retain and finish the dub. A second tap
  stops playback; it does not abort the ending dub.
- `REC > DUB` counts as created by the record-finish tap. A quick second tap may
  discard that new automatic dub while retaining the completed first take.

## Musical timing

Actions use ticks with 16 fractional bits. `actionPositionQ16()` includes the
metronome offset and counts future pulses for bar/half/quarter intervals, or
selects the next half-click, quarter-click, or eighth-click subdivision for
1/8, 1/16, or 1/32 bar. Triplets retain fractional positions. Start and Stop
both default to Synced, straight 1/16 bar. `finalizeAudioWindow()`
shortens an audio render window so `processTick()` reaches the deadline at the
first audio sample on or after it.

Each capture stores `captureStartPositionQ16`. A first-take length is calculated
from the requested Q16 end minus that exact start, rounded to the nearest whole
tick and clamped to `[1, kMaxLoopSequenceLength]`. Therefore a take from tick
2.5 to 22.5 has length 20, not 21. Fixed duration stores exactly:

```text
capture start Q16 + duration bars * bar length Q16
```

The WAV frame count is determined by actual audio capture. Phrase tick length and
WAV frame count are separate values. Overdubs always retain the first take's
phrase tick length and frame count.

## Capture startup and failures

Startup is guarded against re-entry on the same track:

```text
preflight -> reserve history -> create recorder -> configure capture -> enter state
```

Preflight returns a typed error:

- clip allocation failure → `ALLOCATION`
- recorder already attached or pool full → `RESOURCE_LIMIT` ("Loop recorder channels full" / `FULL`)
- missing backing for overdub → `BACKING_UNAVAILABLE`
- removed or invalid input → `SOURCE_REMOVED`
- invalid first-take state → `INTERNAL`
- recorder or SD setup failure → the mapped allocation/storage error

The shared recorder budget has two limits. At most 16 recorder objects may be
retained across setup reservations, active captures, sealed writers, and
recorder-backed playback providers; durable history metadata is excluded. A
separate 16 channel-unit limit covers active capture: mono physical first takes
cost one unit, while stereo first takes and overdubs cost two. Loop Station
capture units release when actual input capture ends, including initial latency
compensation. Ordinary recorder channel accounting keeps its existing
destruction-based lifetime. Admission happens before setup can yield, and a
limit rejects the take as a resource error while preserving prior audio.

## First take

### Start and audio capture

1. Create or reuse the bound Audio Clip.
2. Reserve the next history entry before recorder allocation.
3. Start a mono or stereo linear recorder for the configured physical input.
4. Disable margins, normalization, channel alteration, and automatic deletion.
5. Apply input lag compensation and enter `RECORDING`.
6. If Record Duration is nonzero, arm its exact Q16 end.

The audio callback writes 24-bit PCM from the physical RX buffer. Each Loop
Station initial take allows at most eight unwritten clusters, including its
current cluster. A cluster under an SD write remains charged until the write
succeeds. The ninth allocation is rejected before allocation with an
insufficient-RAM/resource error; no partial take is published. Card service
creates the clip WAV and writes completed clusters while capture continues.
Maximum-size, allocation, or card errors latch on the recorder and are polled
by the view.

### Finish and provisional playback

Finish can come from a centre gesture, fixed duration, Stop All, transport stop,
or view exit. Stop All sets stop intent; transport stop closes immediately;
normal gestures and exit use Stop Action. Transport stop preserves its exact Q16
endpoint, including when a yielding operation defers completion.

The view rejects zero-frame, max-size, aborted, or failed recorders. Otherwise it:

1. Sets clip and original length from the engine's phrase length.
2. Retains the recorder for the commit transaction.
3. Ends synced capture, including remaining latency-compensation frames.
4. Attaches the working Sample to the Audio Clip.
5. Sets sample-zero to zero and forces sample transpose metadata to zero.
6. Stages the revision and registers its pending recorder.
7. Starts provisional playback unless stop intent is set.

`REC > PLAY > DUB` enters Playing. `REC > DUB > PLAY` starts a dependent overdub
immediately for Loop playback. One Shot never auto-overdubs. The dependent dub
may use the provisional parent Sample while the parent finishes writing.

Card service writes remaining clusters, updates the WAV length, truncates excess
latency data, and closes the file. Only a complete recorder without a latched
error can publish.

## Overdub

### Setup

1. Read the live generation's physical frame, or the sample voice for a first overdub.
2. Convert that frame to logical phrase phase using `sampleZeroFrames`.
3. Create a fixed-length 24-bit stereo recorder.
4. Retain the current source Sample, pin up to four raw-source clusters ahead,
   by walking byte ranges through frame boundaries and loop wrap,
   and allocate an eight-entry destination cache plus one cluster-sized SD write
   snapshot and one cluster-sized pending-input buffer.
5. Rotate source frame zero to the overdub start and enter `OVERDUBBING`.

The pending-input FIFO is freed after sealed input drains. Source ancestry and
source readers are released once the complete working generation is
materialized. The SD snapshot is freed after successful close. A recorder-backed
playback reader is handed off after the normal Audio Clip voice is ready at the
same sample frame; retained recorder admission remains charged until the
recorder object itself is released.

### Per-buffer mixer

The Audio Clip renders neutral backing at native speed and unity capture gain.
For each destination frame, the recorder reads the corresponding PCM frame from
the retained source WAV, independently of the clip render:

```text
first pass:  previous = retained source WAV frame
later pass:  previous = cumulative destination WAV frame
addition = physical input while the input gate is open
result = saturatingAdd(previous, addition)
write top 24 bits of result into the destination cache
```

This keeps a silent overdub byte-stable at 24-bit storage precision and prevents
playback transpose or attack envelopes from accumulating across overdubs. A
missing working frame at its audible deadline fails the provisional take rather
than substituting older audio. No SD write or allocation occurs in this
per-frame loop.

Source prefetch is independent from the active Audio Clip voice. A pending read
is distinct from invalid backing and an SD operation failure. While source or
destination writes await their cache, input is retained in a preallocated FIFO
and mixed against the raw source when ready. A late-starting playback voice does
not latch a capture error. Closing input retains already accepted frames, and
completion waits for them to drain. FIFO exhaustion reports insufficient RAM;
it never silently drops accepted input or stores an unverified playback buffer.
Catch-up work is limited to 128 queued frames per service call plus one per new
audio frame.

The recorder also returns the current cumulative backing to audible playback on
**every pass**, including while an early-stop tail is still copying. The production
pipeline then applies the playback envelope and stop fade, adds the optional live
monitor, and pitches the combined stereo buffer once before track FX. Monitoring
off removes only the live return; earlier recorded additions keep repeating.

Each output owns one stereo buffer-fed pitch processor, allocated during setup.
Monitoring refreshes and transpose edits retain its history. Unity pitch bypasses
exactly; a real render gap or return from bypass resets its heads without allocating.
The processor reads the supplied mix, never the hardware input directly. Nonzero
pitch uses the live shifter's adaptive read heads, so audible latency and transient
quality still require hardware checks.

### Input close and untouched tail

Stopping an overdub closes its input gate immediately and shows Playing. If less
than one full pass was captured, card service copies the untouched tail from the
retained source WAV in batches of at most 128 frames. Source or destination data
that is still loading causes a retry without advancing the cursor.

The recorder enters background finalization as soon as input is sealed. Card service drains
dirty cache entries one at a time, using the pinned snapshot so audio can dirty a
live entry while a write yields. Exact seek and write lengths are verified; the
file is closed before the recorder becomes Complete.

One continuous overdub creates one cumulative history revision, regardless of
how many times it wrapped.

If the performer requests another overdub while an early-stop tail is still being
materialized, the engine starts a dependent capture on that gesture. Source
resolution follows sealed generations to the recorded prefix or original tail;
it never treats an unmaterialized destination as silence. Live playback owns its
sample cursor independently of VoiceSample startup and save completion.

## Commit, rollback, and SD errors

The old durable revision, staged revision, recorder hold, and any dependent dub
retain their Samples until resolution. On completion:

1. Examine the oldest pending generation under the slot's re-entry guard.
2. Publish its saved revision without applying it to the live clip.
3. On failure, detach provisional playback, abort dependent capture, discard
   unpublished generations, and restore the last durable revision.
4. Resume after rollback only if the track should remain audible.
5. Notify the engine and release the commit hold. Any remaining playback or
   source readers retain separate holds only while they need that recorder, so
   publication cannot delete data still in use.

Seek, short write, sync, close, card-full, card-missing, no-filesystem, and
write-protected errors retain their specific cause. A failed revision is never
published. Cleanup waits for voice, history, recorder, and dependent-source
references before deleting a provisional Sample or temporary WAV.
A recorder whose revision has already published is durable: a later playback
prefetch failure retires the live provider and reports the error, but never sends
that WAV through aborted-recording cleanup or unlinks it.

## Stop All, transport stop, and exit

**Stop All** cancels queued starts/retriggers. Recording finishes with Stop Action.
Overdub input closes immediately, then playback stops with Stop Action. Playing
also stops with Stop Action. A pending SD commit continues in the background and
does not force an immediate stop.

**Transport stop** clears scheduled actions, immediately closes active captures,
silences existing phrases, and records deferred stop intent if a yielding setup or
finish guard is active. Tail copying and commit publication continue.

**Exit** cancels queued starts, closes overdub input, and finishes first takes with
Stop Action. It suppresses automatic `REC > DUB` and waits until active captures,
tail copies, scheduled actions, and commits drain.
Save and Load requests during capture use the same drain. The first request is
retained with its original press sample, optional matching release sample, and
Shift state; later requests do not replace it. Once capture and commits drain,
the original short/long hold and modifier intent are replayed once. A press
still held at drain completion is dispatched then, and its later physical
release completes the same global action. Shift+Load retains its clear-song
intent even if Shift changes during the drain. On exit,
the completed live generation hands its phase back to ordinary clip playback
before releasing its reader hold.

## Transpose and song persistence

Track transpose is authoritative and stored as `transpose` plus `transposeCents`
in each Loop Station song track. Values are canonicalized without losing any
cent value in the range -24.50 to +24.50 semitones. Zero is explicit and takes
effect immediately.

Loop Station sample metadata remains neutral. During audible playback,
`AudioClip` renders at native speed; the owning `AudioOutput` applies track
transpose to its combined backing and monitor without changing the recorded frame
count or phrase duration.
For an older song without track transpose fields, opening Loop Station migrates
the bound Audio Clip's transpose into the track once, then clears the sample
metadata. A legacy stale clip value cannot be distinguished from an intentional
setting, and pitch already baked into an old WAV cannot be reconstructed.

## Other edge cases

- Undo/redo and Clear are blocked while capture, tail completion, or commit is active.
- A new successful revision truncates redo only when it publishes.
- Parent commit success leaves a dependent dub running; parent failure aborts it.
- One Shot ends from the audio voice, not a tempo-derived tick.
- Playback restart uses logical phase zero; mid-loop overdubs keep their rotated
  physical zero in `sampleZeroFrames`.
- Internal-source enum branches exist, but current sanitization exposes only
  Left, Right, Stereo, and Balanced.
- Acknowledging an error performs no record/play action; the next gesture does.

Regression coverage includes the production buffer pipeline with real recorder
code (cumulative replay, dry storage, monitoring and gain order), source-cluster
boundaries/wrap, and direct/deferred fractional transport stops. Tests use an
injected pitch transform; they do not validate the ARM shifter's acoustic quality.
Additional coverage is in the Loop Station action/integration tests, recorder
lifecycle/finalization tests, and loop-storage tests. Hardware checks are still
required for eight simultaneous stereo tracks on supported slow SD media, long
phrases across cache windows, rapid dependent overdubs, both Record Actions,
tempo changes, monitoring off, transpose extremes, transport stop, Save/Load
during capture, saved/reloaded undo history, and audible continuity on OLED and
seven-segment models.
