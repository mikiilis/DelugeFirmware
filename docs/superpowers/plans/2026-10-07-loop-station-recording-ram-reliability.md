# Loop Station Recording, Overdub, and RAM Reliability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve immediate recording and overdub gestures while bounding temporary RAM use, maintaining sample phase, and safely publishing or rejecting every generation.

**Architecture:** Keep musical timing in Engine, capture/storage in SampleRecorder, and clip/history coordination in LoopStationView. Separate capture admission, queued-input readiness, playback ownership, and durable publication so their lifetimes cannot accidentally substitute for one another.

**Tech Stack:** Firmware C++23, CppUTest host tests, UBSan, FatFS storage tests, ARM firmware build.

**Spec:** `LOOP_STATION_SPEC.md`; implementation reference: `LOOP_STATION_RECORDING_OVERDUB_FLOW.md`. Apply the explicitly selected amendments below to the specification.

## Summary and review findings

The changes generally support the intended behavior: immediate overdub input closure, cumulative dry audio, independent playback cursors, retained generation ownership, ordered history publication, and protection against deleting published WAVs.

The principal specification conflict is recorder admission. The specification charges recorders through finalization and ownership cleanup; the implementation now excludes background writers and even commit-held recorders still capturing latency-compensation frames. Bounded overlap is the selected replacement policy.

Verification performed:

- All five relevant host suites passed with UBSan: UnitTests, RecorderLifecycleTests, RecorderFinalizationTests, LoopStorageTests, and LoopRevisionPublicationTests.
- The configured Debug firmware build completed successfully.
- `git diff --check` passed.
- Hardware audio continuity, SD throughput, and real allocator behavior remain unverified.

The following findings come from static tracing; they are not claims of reproduced hardware failures.

| Priority | Finding | Consequence |
|---|---|---|
| P1 | [Recorder admission](/Users/mikiilis/Documents/code/DelugeFirmware/src/deluge/processing/engines/audio_engine.cpp:1562) excludes saving recorders without a shared retained-recorder limit. | Per-slot limits permit 128 pending generations, substantially increasing memory and SD workload. Ordinary recorder clients also receive changed admission semantics. |
| P1 | Initial recording allocates additional unwritten clusters without a backlog limit. | A slow writer can consume available RAM rather than fail at a predictable boundary. |
| P1 | [Source prefetch](/Users/mikiilis/Documents/code/DelugeFirmware/src/deluge/model/sample/sample_recorder_loop.cpp:170) follows the writer cursor; audible destination prefetch follows playback separately. | During early-stop tail copying, playback can outrun source prefetch. Borrowing resident descriptors hides this until clusters are evicted. |
| P1 | Source resolution treats any parent queued input as blocking, while playback readiness relies mainly on first-pass materialization. | Unrelated pending frames can reject a dependent take; pending additions across short-loop wraps need stronger checks against stale audible audio. |
| P1 | [Abort and rollback](/Users/mikiilis/Documents/code/DelugeFirmware/src/deluge/gui/views/loop_station_view.cpp:514) do not consistently preserve sample phase. | A linear parent bypasses the explicit abort-resume branch; rollback derives phase from musical ticks, which can disagree after tempo changes. Failed restoration also lacks the previous rejected-sample detachment safeguard. |
| P2 | Completed providers retain capture buffers and source ownership until later destruction. | Pending-input storage, write snapshots, source Samples, and unnecessary cache pins remain allocated during ordinary playback or stop. |
| P2 | Deferred Save/Load replays button events with new timestamps and current modifier state. | Long holds can become short presses; a changed Shift state can alter the requested action. |

## Global constraints and selected defaults

- Preserve **8 independent tracks** and **16 channel units**.
- Introduce a shared maximum of **16 retained recorder objects**, counting setup reservations, active captures, sealed writers, and recorder-backed playback providers. Durable history metadata does not count.
- Loop Station channel units release only when actual input capture ends, including initial-take latency compensation. Ordinary recorder clients retain their existing lifetime accounting.
- Bound each Loop Station initial take to **8 unwritten clusters**, including the current cluster. A cluster being written remains charged until its write succeeds.
- Budget exhaustion rejects the take explicitly, preserves preceding audio, and never publishes partial audio.
- Preserve original phrase frames, native playback speed, logical sample zero, dry saturating accumulation, and one revision per overdub session.
- Retain the eight-entry destination cache, four-entry source cache, cluster-sized write snapshot, and bounded input FIFO.
- No allocation or SD write in the per-frame cumulative mixer.
- No new RAM-first recording mode and no song XML format change.

At 32 KiB clusters, sixteen overdubs’ fourteen cluster-sized buffers have a conservative upper estimate of **7 MiB**, excluding metadata and other audio resources. This is a resource bound, not a hardware throughput guarantee.

## Review focus

1. Eight active stereo takes with eight preceding generations still saving: admit all sixteen retained objects and reject the seventeenth safely.
2. Playback outrunning early-stop copying with evicted source clusters: prefetch the audible source window and preserve valid backing.
3. Queued additions spanning multiple short-loop passes: retain order and never present stale cumulative audio as current.
4. Abort or rollback after tempo changes or against a linear parent: retain sample phase and stop intent.
5. Save/Load during yielding finalization: preserve the initiating gesture and execute it once after draining.

## Implementation tasks

### Task 1: Make recorder admission atomic and globally bounded

**Files:** Create `src/deluge/processing/engines/recorder_budget.h` and `tests/unit/recorder_budget_tests.cpp`. Modify AudioEngine recorder allocation, SampleRecorder configuration/lifecycle, AudioClip recording startup, and LoopStationView error mapping.

**Interfaces:** Add a move-only `RecorderBudget::Lease`, returned by `tryAcquire(uint8_t channels, bool loopStation)`. Expose `retainedCount()` and `channelUsage()`; `Lease::finishCapture()` releases Loop Station capture units without releasing its retained-object reservation. Destruction releases remaining charges once.

- [ ] Write tests for eight sealed stereo leases plus eight active stereo leases, seventeenth-object rejection, ordinary-recorder accounting, and recursive admission during setup.
  
  Required assertions: `CHECK_EQUAL(16, budget.retainedCount())`, `CHECK_EQUAL(16, budget.channelUsage())`, and rejected admission leaves both counts unchanged.
- [ ] Run `UnitTests -g RecorderBudget`; confirm the new cases fail.
- [ ] Acquire the lease before allocation or setup can yield. Move it into SampleRecorder and release it on setup failure or destruction. Add `RecorderConfig::loopStation`; set it from the bound clip. Do not use `isCommitPending()` as evidence that capture ended. Propagate admission rejection distinctly from allocation or filesystem failure.
- [ ] Rebuild UnitTests and recorder suites; confirm the admission tests and existing lifecycle cases pass.
- [ ] Commit as `fix: bound retained recorder admission`.

### Task 2: Bound initial-recording RAM backlog

**Files:** Modify `sample_recorder.cpp`, `sample_recorder.h`, and recorder writer bookkeeping. Add coverage to the recorder lifecycle/finalization targets.

**Interfaces:** Keep `Error SampleRecorder::createNextCluster()`. Add `kMaxLoopUnwrittenClusters = 8`, applied only to Loop Station linear capture.

- [ ] Write tests for the eighth permitted cluster, rejection before allocating the ninth, successful-write capacity recovery, and allocation attempted during an in-flight write. Assert no allocation on rejection, `INSUFFICIENT_RAM`, no invented card error, and balanced cleanup.
- [ ] Run the affected recorder tests and confirm the new backlog cases fail.
- [ ] Check the bound before changing cluster ownership or allocating the next cluster. Include in-flight writes in occupancy until successful completion. Route exhaustion through existing terminal capture failure and rollback.
- [ ] Run lifecycle and finalization suites. Exercise mono/stereo frames crossing cluster boundaries and final latency-compensation frames. If needed for host coverage, extract `createNextCluster()` into a focused production translation unit rather than duplicating it in a fixture.
- [ ] Commit as `fix: bound loop recording write backlog`.

### Task 3: Make source readiness follow audible frames

**Files:** Modify `sample_recorder_loop.cpp`, its declarations, and `audio_pipeline.h`. Extend `recorder_loop_tests.cpp` and its cache fixtures.

**Interfaces:** Preserve `processLoopOverdub(...)`, `locateLoopSource(...)`, and the READY/PENDING/INVALID distinction. Add `uint64_t loopAcceptedFrames_` and `loopAppliedFrames_` to track ordered queued input across wraps.

- [ ] Write production-pipeline regressions for playback overtaking copying, split PCM frames, rotated generation boundaries, and pending parent input affecting only part of a phrase. Force released clusters to disappear and loads to complete asynchronously.
- [ ] Run `RecorderLifecycleTests -g LoopRecorder`; confirm these cases expose current readiness/prefetch behavior.
- [ ] Deduplicate audible and writer source windows within four source pins, prioritizing the current audible frame and split-frame companion. Resolve availability for the requested frame rather than blocking on all parent pending input. Use accepted/applied serial ranges to detect pending additions required by a replay deadline without scanning the FIFO per frame.
- [ ] Verify chronological saturation, monitoring off, multiple wraps within one buffer, and immediate input closure. Capture-side waiting must retain input; stale or missing working audio must reject the provisional take. When preceding backing remains readable, continue it at the matching phase during rejection.
- [ ] Commit as `fix: track overdub source readiness by frame`.

### Task 4: Release temporary resources at their actual milestones

**Files:** Modify SampleRecorder loop, card, and lifecycle implementations; extend recorder lifecycle tests.

**Interfaces:** Add private, idempotent `releaseLoopCaptureBuffers()`, `releaseLoopSourceReaders()`, and `releaseLoopWriteSnapshot()`. Introduce a persistent raw-source playback mode flag so playback does not depend on `loopSource_` remaining non-null.

- [ ] Write tests asserting FIFO release after sealed input drains, snapshot release after successful close, ancestry release after full materialization, and preservation during yielding writes.
- [ ] Run the new cases and confirm retained allocations/source reasons exceed the intended post-milestone values.
- [ ] Release each resource at its milestone. Fully materialized generations resolve all frames from their own Sample, allowing ancestry to retire even with dependent readers. Unpin clean cache entries outside required windows; never evict dirty or in-flight data.
- [ ] Run 100 successive production-style parent/child handoffs using `sourceRecorder`, plus delayed-publication and abort-order tests. Assert resource use reaches a stable plateau and cleanup never unlinks a published WAV.
- [ ] Commit as `fix: retire temporary overdub resources promptly`.

### Task 5: Preserve phase and ownership through publication and rollback

**Files:** Modify LoopStationView capture/commit handling, AudioClip playback-provider handling, and revision recovery. Extend recorder, revision-publication, and engine integration coverage.

**Interfaces:** Add frame-domain phase conversion helpers. Change `resumeCommittedClip` to accept an explicit logical sample frame. Retain the existing slot transaction guard and oldest-first publication.

- [ ] Write tests for aborting an automatic dub with a linear parent, rotated phase rollback after a tempo change, durable provider handoff, failed durable restoration, and stop intent during a yielding rollback.
- [ ] Run the affected cases and confirm failure or missing coverage in the current production coordination.
- [ ] Snapshot logical sample phase before detachment. Translate it using the restored revision’s `sampleZeroFrames`; never derive recovery phase from current tempo. On restoration failure, detach the rejected Sample while retaining durable history for retry and preserve the initiating failure separately from recovery failure.
- [ ] Handoff a durable provider to ordinary clip playback once the replacement voice is ready at the matching frame. Preserve envelope and pitch history; release provider ownership afterward. Verify older publications cannot replace newer live audio, failed children retain successful parents, and failed generations preserve redo.
- [ ] Commit as `fix: preserve loop phase during generation recovery`.

### Task 6: Preserve deferred global gestures and reconcile documentation

**Files:** Modify LoopStationView deferred-action handling and add a focused gesture-policy helper with unit coverage. Update the specification, recording flow, architecture README, and user guide.

**Interfaces:** Store the button, original press sample, optional release sample, and Shift state in one deferred gesture record. The first pending request wins; matching release events update that request.

- [ ] Write tests for short press, long hold, held-through-drain release, Shift changing during drain, Shift+Load clear-song intent, and repeated background completion.
- [ ] Run the new gesture-policy cases and confirm current replay cannot preserve their expected classification.
- [ ] Dispatch the captured intent once after capture and commits drain. Preserve original hold duration and modifier intent instead of synthesizing a fresh click using current hardware state.
- [ ] Update documentation with the approved sixteen-object overlap limit, eight-cluster initial backlog, resource-release milestones, and remaining hardware requirements. Remove conflicting claims about charging all Loop Station writers through finalization.
- [ ] Run final verification and commit as `fix: preserve deferred loop station global actions`.

## Final validation and handoff

Run the five host suites with UBSan and build Debug firmware. Extend Linux 32-bit revision ownership coverage to `publishGeneration()` using the real String and allocator; native fixtures are not a substitute. Run ASan on a host where it starts successfully.

Hardware acceptance must cover eight simultaneous stereo tracks, slow supported SD media, long phrases crossing cache windows, rapid dependent overdubs, both Record Actions, tempo changes, monitoring off, transpose extremes, transport stop, Save/Load, and saved/reloaded undo history. Record buffer/queue high-water marks, write latency, allocation counts, and audible discontinuities.

Acceptance requires correct cumulative WAV bytes, ordered history, predictable admission failures, phase-preserving transitions, and temporary resource counts returning to baseline after draining.

**Plan artifact:** Save as `docs/superpowers/plans/2026-10-07-loop-station-recording-ram-reliability.md` when execution is enabled. No tracked files were edited or plan file written in Plan Mode.
