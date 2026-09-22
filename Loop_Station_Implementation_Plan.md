# Loop Station Start/Stop refactor

Implementation: 2026-09-08. Canonical behavior is in
[LOOP_STATION_SPEC.md](LOOP_STATION_SPEC.md).

## Settings and migration

Replace Record Quantize, Start/Stop Timing and Restart Behavior with Start Action
and Stop Action submenus. Each offers Immediate/Synced and twelve intervals:
bar, half, quarter, eighth, sixteenth, thirty-second, and triplets of each. Retain
Record Action. Remove track transitions, Fade Time and Loop Sync. Default both
Start and Stop to Synced, straight 1/16 bar. Preserve saved action settings:
interval IDs 0–7 retain their meaning; append straight 1/16 and 1/32 as IDs 8–9
and their triplets as IDs 10–11. Persist four mode/interval fields; ignore removed
legacy fields while preserving audio, bindings, history and unrelated settings.

## Scheduling and gestures

Use one future-pulse calculation for recording, playback and group controls.
Count four future metronome clicks for bar, two for half, one for quarter, or
the next half-click for eighth, quarter-click for sixteenth, or eighth-click
for thirty-second. Triplet pulses have 2/3 spacing. Exact-boundary
gestures wait for the following pulse. Preserve fractional timing through audio
window splitting. Pending LEDs clear on execution/cancellation. Keep the
stopped release-to-start and two-second hold-to-clear gestures.

Stop All covers recording, overdub and playback, cancelling queued starts.
Close overdub input immediately; apply Stop Action to backing playback. Stop
transport without stranding pending captures or background commits.

## Audio and capture

Keep recorded frame length and native playback speed independent of tempo.
Restart stopped playback from logical beginning. Store cumulative sample-zero
phase in frames and migrate old tick offsets. Derive progress and one-shot
completion from sample playback.

Implement continuous additive overdub using a bounded cluster cache and SD
snapshot. Preserve earlier passes, close input immediately, and copy untouched
first-pass backing in background even when playback stops. Keep working samples
separate from durable history; publish one revision per entire overdub session.
Enter Play/Dub at initial capture completion without another musical wait.

## Refactoring and documentation

Remove the unused parallel core model/engine and its duplicate tests. Update the
operational engine tests, recorder lifecycle harness, user guide, architecture
README and specification. Mark historical timing analyses as superseded.

## Validation

Run UnitTests, RecorderLifecycleTests and RecorderFinalizationTests, build debug
firmware, and build the website. Cover future pulses, triplets, fractional
boundaries, cancellation, Stop All, transport shutdown, asynchronous publication,
cumulative wraps within one buffer, input gating, yielding SD writes and rollback.

Hardware acceptance is still required: eight simultaneous stereo tracks, slow
SD cards, external clock, rapid tempo changes, short loops, sample progress,
audio continuity and both OLED/seven-segment menu layouts. Host tests and a
successful cross-compile do not establish those hardware properties.

## Verification results (original 2026-09-08 refactor)

- UnitTests, RecorderLifecycleTests and RecorderFinalizationTests: passed.
- `./dbt build debug`: passed.
- Website `pnpm run build`: passed, 64 pages and valid internal links.
- Loop Station guide formatting and `git diff --check`: passed.
- Hardware acceptance: not performed.

The website's existing Sharp native binary was rebuilt locally to run image
generation; dependency declarations were unchanged. Unrelated generated menu
tree changes were excluded from this refactor.
