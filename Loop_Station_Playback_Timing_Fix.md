> Historical analysis: timing, phase and overdub behavior described here is superseded by the 2026-09-08 [Start/Stop refactor](Loop_Station_Implementation_Plan.md) and [current specification](LOOP_STATION_SPEC.md). Retained for the original failure investigation.

# Loop Station playback timing correction

## Findings

The independent Start Timing and Stop Timing menu fields and engine branches were correctly connected. The host state tests passed because they supplied integer ticks directly and did not exercise the view/scheduler handoff.

The integration had two clock-origin errors:

- The graphics routine called the same action-processing method as Session's musical scheduler. Interpolated musical time could execute queued actions during graphics refresh or ahead of the scheduler tick being processed.
- A delay measured from that interpolated time was written directly into `swungTicksTilNextEvent`, whose origin is `lastSwungTickActioned`. Graphics could change this interval without rescheduling the timer through the playback handler.

For example, a target at 96, scheduler position 24, and UI position 40 require a scheduler delay of 72, not 56. During catch-up, UI position 100 must not execute that target in the callback for tick 24.

Separately, discarding fractional gesture ticks made a tap just after a bar boundary look exactly on the boundary and eligible for immediate execution.

These are source-confirmed defects. The precise cause of the reported physical-device regression has not been established with a device trace.

## Implementation

- `scheduler.h` contains the production scheduler handoff, also exercised by host tests. It executes actions at the supplied scheduler tick and calculates the remaining delay from that same origin.
- Only Session's scheduler calls this handoff. Graphics performs commit/error/monitoring housekeeping and requests scheduler attention using `PlaybackHandler::expectEvent()`; it neither executes queued musical actions nor writes the scheduler interval.
- The view preserves any earlier deadline installed by engine delegate callbacks or other clip work.
- Fades request musical ticks, and one-shots advertise their phrase-end deadline. Neither depends on graphics cadence. A catching-up scheduler cannot underflow a newly started one-shot's position.
- A fractional-boundary flag is preserved from gesture time. Synced actions after a bar line wait for the following bar; exact-boundary actions and transport startup at tick zero still execute there. Recording quantization and gesture dispatch are unchanged.
- Start Timing / Stop Timing XML persistence and legacy migration are unchanged.

## Diagnostics

`deluge::loop_station::schedulerTrace` is a 32-entry RAM ring. `schedulerTraceSequence` is its insertion count. Each entry records the slot, pending action and target, scheduler tick, observed musical tick, bar length, and both timing settings before scheduler processing. A target greater than the recorded scheduler tick must remain pending. The ring is bounded, uses no allocation, and prints/writes nothing to the SD card. It is intended for debugger inspection, not as a persistent log.

## Automated coverage

Eight added tests exercise the production handoff and engine:

- Scheduler-origin delays and preservation of earlier metronome/clip deadlines.
- Catch-up with UI time beyond a queued start/stop target.
- Fractional bar boundaries in both timing modes.
- Fade progression and one-shot completion without graphics.
- Ignoring committing deadlines without a zero-delay spin.
- Immediate one-shot start while the scheduler is behind the gesture.
- All four timing combinations over multiple bar resolutions using single-start/double-stop gestures.

The existing exact-boundary, transport-zero, recording, ownership, and timing-parameter tests remain in place. These host checks do not simulate a physical MIDI clock, actual menu interaction, or the complete song XML reader.

## Hardware acceptance checklist (not yet performed)

1. Use a saved song with different-length loops, Instant transitions, and both timings Synced. Start and stop halfway through a bar: pending indication must persist until the next bar, for centre gestures and sidebar buttons.
2. Test all four Start Timing / Stop Timing combinations. Immediate actions must have no musical wait; synced actions must not depend on individual phrase length.
3. Test just before, exactly on, and just after a bar line. Test a stopped transport separately: its initial tick zero is an eligible boundary.
4. Repeat with metronome off/on, sparse Session events, swing, tempo changes, and external MIDI clock. Inspect the RAM trace if the symptom remains.
5. Repeat with Start/Stop Transition Fade. Fade must begin at its selected boundary, finish after Fade Time, and continue while a settings menu is open.
6. Save/reload each timing combination; load an older song with shared `playbackTiming`; confirm the existing migration behavior. No serialization was changed by this correction.
7. Rapidly create short loops, overdub, undo/redo, and leave during a commit. Confirm the prior recorder ownership/error behavior is preserved.
