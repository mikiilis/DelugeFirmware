# Loop Station architecture

`loop_station.h/.cpp` is the operational firmware-independent model and engine.
It owns eight slots, sanitized settings, gestures, pending actions, revision
navigation, geometry and routing helpers. Side effects cross `EngineDelegate`.
The unused parallel `deluge::loop_station::core` implementation was removed.

`LoopStationView` binds the engine to ordinary Audio Clips and delegates capture,
input gates, playback, errors and durable revision publication. Song XML owns
settings and history; WAV files hold audio. Menus use bounded parameter adapters
shared with host tests. Song interval encoding is independent of menu order and
preserves existing straight/triplet IDs.

Start Action and Stop Action share one deadline calculation for individual and
group gestures. Both default to Synced, straight 1/16 bar. Intervals range from
1 bar to 1/32 bar, with triplet variants of each. Twenty-fourths-of-a-tick
arithmetic retains straight and triplet pulse ratios; Q16 positions preserve
fractional deadlines. `AudioEngine` splits render windows at those deadlines. Display refresh never executes queued actions.
`scheduler.h` retains the integer scheduler adapter and bounded diagnostic trace
for host coverage; production action execution uses the audio-window hook.

Bound Audio Clips play at native speed. Sample frames own phrase length,
playback progress and the logical phase represented by physical sample zero.
Changing tempo affects future action deadlines, never existing audio duration.

`sample_recorder_loop.cpp` implements continuous overdub into a working WAV.
An eight-cluster cache and a single SD snapshot bound its audio memory use.
Subsequent passes read the cumulative data; every pass adds dry physical input.
Dirty/in-flight entries remain pinned. Closing the input gate is immediate;
untouched first-pass backing can finish copying in card work with playback off.

Recorder admission reserves from a shared limit of 16 retained objects. The
count includes setup reservations, active captures, sealed writers, and
recorder-backed playback providers, but excludes durable history metadata. A
separate 16-unit limit applies to active capture; Loop Station capture units
release when input ends, including initial latency-compensation frames, while
ordinary recorder channel accounting retains its existing lifetime. Each
initial Loop Station take also stops at eight unwritten clusters, counting the
current and in-flight-write clusters. The next cluster is made available only
after a successful write.

Temporary overdub resources retire at their milestones: the input FIFO after
sealed input drains, source ancestry and readers after full materialization, the
write snapshot after successful close, and the playback provider after the
ordinary clip voice is ready at the matching frame. A working WAV may be audible
before it is durable. History publication is transactional, does not restart
playback, and retains the previous durable revision for rollback. One overdub
session produces one revision, regardless of pass count.

`commit.h` coordinates publication and rollback under the slot's capture guard.
On failure it gates and aborts the dependent recorder, detaches playback/capture,
restores durable history, and notifies the engine before releasing the parent's
commit hold. Successful publication leaves an immediate dependent overdub running.
Aborted recorder cleanup releases writer resources once and waits, without blocking,
for remaining Sample owners before deletion. Clip/song removal releases pending
commit holds, and temporary-sample garbage collection skips owned Samples.
Dirty cache writes rotate through entries; missing backing is traced as an
underrun instead of being mislabeled as a filesystem error.

See [the E282 investigation and implementation record](../../../Loop_Station_E282_Investigation_Plan.md)
for the reproduced ownership failure, regression coverage and remaining device checks.

See [the specification](../../../LOOP_STATION_SPEC.md) and
[implementation plan](../../../Loop_Station_Implementation_Plan.md) for behavior,
migration, validation and hardware acceptance requirements.

`storage.h` contains the shared checked snapshot transaction used by overdub
writers and the production-FatFS host harness. It verifies seek/write counts and
syncs partial sectors before raw-reader visibility. The first failure stage and
filesystem result survive cleanup in recorder diagnostics. Active failure handling
uses a reentrancy guard; abort detaches the output route immediately while Sample
and cache ownership continue through deferred cleanup.

After overdub input closes, cache prefetch follows both the independent audible
cursor (four clusters ahead) and the copy cursor (two ahead), deduplicated within
the eight-entry budget. Background copying waits for a complete destination batch
instead of turning temporary cache pressure into a terminal backing error.

Capture startup shares the per-slot transaction guard with finishing and commit
publication. Exit waits for that guard, and transport-stop intent persists across
yielding startup. Reopening the view must preserve live engine/recorder ownership.
Loading or clearing a song during capture first drains an exit; retry Load from
Song View afterwards.

`revision.h/.cpp` own cumulative revision history and Sample reasons.
`diagnostics.h` provides a RAM-only ownership context and `lastStringFault` for
S001 investigation. The real String/allocator tests live in
`tests/32bit_unit_tests/loop_revision`; run them on Linux with multilib support.
