> Historical analysis: timing, phase and overdub behavior described here is superseded by the 2026-09-08 [Start/Stop refactor](Loop_Station_Implementation_Plan.md) and [current specification](LOOP_STATION_SPEC.md). Retained for the original failure investigation.

# Short-loop error and freeze: analysis and fix plan

Analysis date: 2026-09-07. Source revision: `f8ae1425`.

## Scope and conclusion

Assumption: the reported workflow is quickly creating short recordings with Loop Station pads. The exact displayed error, hardware revision, recording settings, and reproduction sequence have not yet been supplied. No device crash was reproduced during this analysis.

The source contains concrete recorder-lifetime defects capable of turning recording completion or a recoverable allocation failure into memory corruption or a freeze. These are stronger initial suspects than loop duration or SD throughput alone. Their presence is confirmed by source inspection; which defect caused the reported device freeze remains unconfirmed.

Short takes repeatedly exercise allocation, asynchronous finalization, abort, and destruction. External recording also captures a latency-compensation tail after the stop request (294 samples, plus 2048 samples if post-end margins are enabled). A stopped take can therefore still allocate clusters and fail while Loop Station already holds its provisional sample.

## Findings

### 1. Recorder destruction dereferences a nullable output pointer — highest priority

`SampleRecorder::~SampleRecorder()` unconditionally calls `outputRecordingFrom->removeRecorder()` (`src/deluge/model/sample/sample_recorder.cpp:55`). However:

- Physical-input recording can supply a null output pointer through `AudioClip::beginLinearRecording()` (`src/deluge/model/clip/audio_clip.cpp:149`).
- Successful output recording calls `finishCapturing()`, which removes the recorder from its output (`sample_recorder.cpp:912`). `Output::removeRecorder()` calls `SampleRecorder::removeFromOutput()`, which sets `outputRecordingFrom` to null (`src/deluge/model/output.h:216`, `sample_recorder.h:74`).
- Later, the global recorder routine invokes the destructor (`src/deluge/processing/engines/audio_engine.cpp:1496`).

Thus even normal cleanup can call a member function through null. This is undefined behavior; its device symptom depends on the compiled code and memory mapping and is not necessarily a named firmware error.

There is also an ownership hazard on failed attachment: if `addRecorder()` rejects a new recorder because the output is occupied, destruction of the rejected recorder calls the output's unconditional removal method and can detach the existing recorder. Cleanup must check identity, not only whether the output pointer is non-null.

### 2. Failed recorder setup leaves a dangling sample pointer — highest priority

In `SampleRecorder::setup()`, the `gotError` branch destroys and frees the allocated sample without clearing the recorder's `sample` member (`sample_recorder.cpp:127`). This branch is reachable when sample initialization or first-cluster allocation fails.

`AudioEngine::getNewRecorder()` then destroys the partially initialized recorder (`audio_engine.cpp:1586`). Its destructor sees a non-null sample and calls `detachSample()`, accessing freed memory. Depending on the point of failure, cleanup also relies on cluster bookkeeping that setup has not completed.

This is a confirmed use-after-free path under allocation failure. It can corrupt sample/allocator state instead of returning the intended insufficient-memory error. An allocator error such as `M123` would warrant investigating this path, but is not proof of this specific cause.

### 3. Abort cleanup can run before provisional sample owners release it — highest priority

Loop Station finishes through `AudioClip::finishLinearRecording()`, attaches the sample to the clip, and retains it in `LoopStationRevisionHistory::stageCurrent()` before WAV finalization finishes (`src/deluge/gui/views/loop_station_view.cpp:410`, `:458`; `src/deluge/model/song/song.cpp:301`).

If a cluster allocation fails during the remaining capture tail, `SampleRecorder::feedAudio()` sets the recorder to `ABORTED` (`sample_recorder.cpp:964`). The next recorder card routine can enter abort cleanup before `LoopStationView::processPendingCommits()` restores the durable revision and discards the provisional one.

The abort branch releases the recorder's own sample reason and explicitly freezes with `E282` if any other sample reasons remain (`sample_recorder.cpp:353`). Its documented assumption—that aborted recordings have never been attached to a clip—is incompatible with the provisional ownership described above. `pointerHeldElsewhere` prevents deletion of the recorder object at the end of this branch; it does not prevent sample cleanup at the beginning.

In builds without that diagnostic, the branch can delete a registered sample still referenced by the clip/history. Removing the assertion would therefore conceal a memory-safety bug.

This is a concrete conditional path to `E282`. Its occurrence depends on failure injection or the relative timing of capture, SD work, and Loop Station rollback.

### 4. The test boundary excludes the failing integration

`tests/unit/loop_station_integration_tests.cpp` uses a fake `EngineDelegate`. `tests/unit/CMakeLists.txt` compiles both Loop Station engine implementations, but does not compile the actual view, `SampleRecorder`, `AudioClip`, or song revision-history implementation.

The firmware view includes `loop_station/loop_station.h`; fixing only the separate `loop_station/engine/engine.cpp` implementation will not fix this operational integration.

Existing tests already check that a pending commit blocks reuse of its slot. Recorder channel usage also retains pending writers until they are released. Preserve these protections: releasing capacity at the stop gesture would increase pressure and would not repair the lifetime defects.

## Ordered implementation plan

1. **Capture a diagnostic baseline.** Record firmware/build type, exact error or fault address, input source, Record Action, quantization, margins, tempo, slot count, and tap sequence. Add a bounded diagnostic trace for recorder creation, finish, abort, rollback, and destruction, including recorder identity, status, sample reason count, output attachment, and pending-commit ownership. Avoid SD writes or allocation from audio callbacks. Match the trace to the source revision above.

2. **Make setup and destruction safe.** Initialize ownership members explicitly. Consolidate setup rollback so each acquired sample/cluster is released once and every released pointer is cleared. Make destruction valid at every setup failure point. Add an identity-aware output detach operation so a recorder can detach only itself; treat absent/already-detached outputs as normal. Cover external-input completion, completed output recording, occupied-output rejection, and every setup allocation failure.

3. **Make abort a two-phase operation.** First stop capture and latch the original error while retaining the recorder, sample, and required clusters. Then let the Loop Station owner restore the durable revision, release playback/clip/history references to the provisional sample, and acknowledge that cleanup may proceed. Only then close/unlink the temporary WAV and destroy sample/recorder resources in a safe non-reentrant card-work context. Keep recorder ownership uninterrupted across `finishLinearRecording()` and staging. Preserve the existing behavior of other recorder clients with explicit ownership semantics rather than repurposing `pointerHeldElsewhere` globally.

4. **Verify transaction and duration boundaries.** Route all finish/stage/publish failures through the same rollback protocol. Preserve the old loop, redo branch, phrase length, and first error; make repeated completion/abort notifications harmless. Test zero captured samples, same-tick taps, one-tick loops, Smart stops near the recording origin/bar boundary, latency tails, and stops around a cluster boundary. Use recoverable rejection for an unusable take. Do not introduce an arbitrary minimum loop duration as a substitute for fixing cleanup.

5. **Add meaningful integration regressions.** Exercise the real recorder lifecycle and revision ownership with controllable allocator/filesystem failures, using host-compatible seams or the firmware test harness as needed. Inject failure during sample initialization, first/later cluster allocation, file creation, write, header update, and close. Force both orderings: recorder card cleanup before owner polling and owner polling before cleanup. Test both alpha/beta diagnostics and release behavior. Use address/undefined-behavior sanitizers for host-compatible lifecycle tests.

6. **Stress-test on hardware.** Repeat short record/play/overdub/stop/clear cycles on one slot and rapidly across all eight; cover mono/stereo physical inputs and output overdubs, both Record Actions, Off/Smart/Measure quantization, margins on/off, and constrained-memory/slow-card conditions. Include retry after an acknowledged failure and exit/transport-stop while commits are pending. Also check ordinary Audio Clip recording and other shared-recorder clients after changing common cleanup code.

## Acceptance criteria

- No null dereference, use-after-free, double cleanup, or removal of another recorder's output attachment in failure-injection tests.
- No `E282` or dangling provisional sample when abort precedes owner polling.
- Failed overdubs keep the previous durable loop and redo history intact; failed first takes return to an acknowledged empty state.
- Every recorder and temporary failed take reaches cleanup; channel usage returns to baseline. Account separately for intentionally retained successful revision history.
- Zero/very short takes either play correctly or fail recoverably, with responsive controls.
- At least 1,000 rapid cycles per selected hardware stress configuration complete without a freeze or growing unreleased recorder count. Log tested settings and latency, and investigate any stuck commit rather than assuming an SD-speed problem.

## Verification performed for this analysis

- Rebuilt the existing host `UnitTests` target successfully.
- Executed all 220 unit tests: 220 passed, 307,878 checks.
- Inspected the real recorder, Audio Clip, output attachment, SD cleanup, revision-history, and operational Loop Station integration paths.
- No firmware source changes, device flashing, or hardware reproduction performed. This document is the investigation and proposed fix plan, not an implemented fix.

## Implementation follow-up

The subsequent implementation addresses the three lifetime defects above:

- Recorder destruction checks output identity and safely handles an absent or already-detached output.
- Sample setup failure clears freed ownership; unregistered failed samples are released explicitly.
- Loop Station retains the recorder continuously across finish/staging and acknowledges cleanup only after publication or rollback. Abort cleanup waits for that acknowledgement and closes the file before unlinking it.
- A failed durable restore detaches the provisional playback sample while retaining durable history metadata for retry. The initiating error remains the reported error.
- A bounded RAM-only lifecycle trace supports hardware investigation without SD writes from callbacks.

Implementation review also found transaction bugs: a finish delegate can yield before the engine reaches COMMITTING, and a background overdub retains `finishingOverdub` while its asynchronous commit is pending. Re-entrant finish processing and repeated completion consumption are now blocked; pending commits are excluded from capture-failure polling and exit rescheduling.

The new `RecorderLifecycleTests` target compiles the real lifecycle and card-routine code with injectable resource/storage boundaries. Additional operational-engine tests cover same-tick and one-tick takes, Smart stops near the origin, re-entrant completion, delayed background commits, duplicate notifications, and redo/error recovery. See [test instructions and hardware procedure](tests/recorder_lifecycle/README.md) for coverage boundaries and remaining hardware checks.

Hardware reproduction/stress testing and real WAV-header/SD fault injection remain unperformed. AddressSanitizer could not initialize on the development Mac; UndefinedBehaviorSanitizer is used for the runnable host lifecycle check. The original analysis above is retained as the pre-fix baseline.

Validated implementation results:

- 226 operational unit tests passed (307,963 checks).
- 17 recorder lifecycle tests passed (26,144 checks) with UndefinedBehaviorSanitizer configured to halt on error.
- The lifecycle stress case completed 1,000 rounds across eight slots: 8,000 aborted captures, with no remaining sample allocations after each round.
- Debug and Release firmware builds succeeded. Build outputs are `build/Debug/deluge.bin` and `build/Release/deluge.bin`; neither has been flashed during this task.
- The existing linker warning about an RWX LOAD segment remains. The Debug build also reports the existing startup-song stack-usage warning outside the changed code.
