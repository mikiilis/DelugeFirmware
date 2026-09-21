# Recorder lifecycle regression tests

This target compiles the production `SampleRecorder` declaration and the production
`sample_recorder_lifecycle.cpp` and `sample_recorder_card.cpp` implementations.
Allocator, sample/cluster storage, output layout, file manager, and filesystem
boundaries are replaced with small fixtures. The production output-detachment
method is also exercised. These fixtures are isolated from other test targets.

Covered behavior includes destruction with no output, failed sample allocation,
sample initialization and first-cluster allocation, rejected output attachment,
retaining a provisional sample across abort, both rollback/card-work orderings,
abort during a file-create yield, file creation/write/finalization errors, closing
before unlinking, repeated cleanup, and successful durable ownership transfer.
The stress test runs 1,000 rounds of eight simultaneous aborts and checks that all
sample allocations are released each round.

The cluster-write and finalization methods below `cardRoutine()` are injected
boundaries, not real disk I/O. WAV header rewriting, physical SD removal, DSP
rendering, real allocator stealing, and full `AudioClip`/song-history integration
still require firmware/hardware validation. The operational Loop Station unit
tests separately cover very short durations, re-entrant finish callbacks, delayed
background commits, duplicate notifications, exit, and preservation of redo.

## Run

From the repository root, with the normal host test dependencies installed:

```sh
cmake -S tests -B build/tests -DRECORDER_TEST_SANITIZERS=ON
cmake --build build/tests --config Debug --target UnitTests RecorderLifecycleTests
ctest --test-dir build/tests -C Debug -R '^(UnitTests|RecorderLifecycleTests)$' --output-on-failure
```

The sanitizer set defaults to `address,undefined`. If the platform cannot start
AddressSanitizer, configure `-DRECORDER_TEST_SANITIZER_SET=undefined` and run with
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. For ordinary unsanitized tests,
configure `-DRECORDER_TEST_SANITIZERS=OFF`.

On the development Mac used for this change, AddressSanitizer crashed before
`main()` in `__asan::InitializeShadowMemory()` / `__pthread_init`; the crash report
reported `BUG IN LIBPTHREAD: Token from the kernel is 0`. Running outside the
sandbox had the same result. This is an uncompleted ASan check, not a passing one.

## Hardware diagnostics and stress procedure

The firmware keeps the last 64 lifecycle events in `recorderLifecycleTrace` when
`ALPHA_OR_BETA_VERSION` is enabled. `recorderLifecycleTraceSequence` identifies the
next entry; traverse from `max(0, sequence - 64)` to `sequence - 1`, modulo 64.
Entries include recorder/sample/output identity, captured frames, sample reason
count, status, terminal error, event, commit ownership, and external pointer ownership. Recording
the trace does not allocate or access the SD card. Inspect it with a debugger at
a freeze, or break at `SampleRecorder::abort` to capture the initiating error.

1. Record firmware hash/build, exact error/fault address, SD card, source, tempo,
   quantization, recording margins, Record Action, and the tap sequence.
2. Test 1,000 rapid record/play/overdub/stop/clear cycles on one slot and then
   across eight slots. Include very short taps, sub-bar loops, and stops around
   bar and sample-cluster boundaries.
3. Cover physical mono/stereo inputs and output overdubs, both Record Actions,
   Off/Smart/Measure quantization, and margins on/off. Verify ordinary Audio Clip
   recording and other recorder clients after the shared cleanup changes.
4. On disposable test material, inject low-memory and storage failures, including
   creation, cluster writes, header update, and close. Verify that failure retains
   the old loop and redo branch, error acknowledgement does not start recording,
   and retry works after cleanup.
5. Request transport stop/exit while a background overdub commits. Check that
   controls remain responsive, the recording finishes only once, all pending
   recorders drain, and channel usage returns to baseline. Successful revision
   history intentionally retains samples and must be counted separately.

No hardware stress result is implied by passing the host stress test.

## E036 finalization regressions

`RecorderFinalizationTests` additionally compiles the production completed-cluster
writer bookkeeping, WAV finalizer, truncation, and `SampleCluster` destructor.
Unlike the original lifecycle target, it does not replace `finalizeRecordedFile()`.
Sample containers, allocation, loading, and physical I/O are still fixtures.
The write boundary provides a sector address; header rewriting runs production
code against memory, with `disk_write` success/failure injected. Audio rendering,
file alteration/normalization, and actual FAT/SD behavior are not simulated.

```sh
cmake --build build/tests --config Debug --target UnitTests RecorderLifecycleTests RecorderFinalizationTests
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build/tests -C Debug --output-on-failure
```

Coverage includes two captured clusters cropped to one while both startup
reasons are retained; mono/stereo and margin headers; exact cluster boundaries;
seek/truncate/header-load/header-write/close errors; final-write/allocation
errors; abort before finalization; rejected playback/loader ownership; ownership
acquired during storage yields; immediate recycling on release; and repeated
trimming. The stress case finalizes four pending recorders per round for 1,000
rounds (4,000 successful host finalizations), checking zero live resources at
completion. A separate engine regression preserves committed history/redo when
an internal finalization error is acknowledged.

For device diagnosis, the recorder ring now includes zero-based Loop Station
slot (`0xFF` for other clients), actual input mode, logical audio byte length,
cluster count/index, total and recorder-held cluster reasons, loading status,
writer indices, retained-startup bitmask, and requested trim size.
`TRIM_CHECK`, `TRIM_REJECTED`, and `TRIM_COMPLETE` identify the trimming path.
`sampleClusterDestructionFailure` preserves the offending cluster, parent Sample,
index, reason counts, and loading flag at E036. Inspect the call stack as well:
whole-Sample destruction and tail trimming must not be conflated.

Repeat the reported four-loop gesture on hardware using a backed-up test song.
Record the binary SHA-256 as well as the displayed version (different dirty
builds may have the same version string), source, quantization, margins, tempo,
and loop duration. Compare Smart with Off and include stops on either side of
cluster boundaries. Verify saved/reloaded loop audio and undo/redo after stress.

## Continuous Loop Station overdub

`recorder_loop_tests.cpp` compiles the production bounded-cache cumulative writer.
It covers additive passes, short loops wrapping repeatedly within one audio buffer,
closing the input gate, preservation of untouched first-pass frames, SD snapshot
updates during yielding writes, and write-failure cleanup. Source copying and SD
callbacks use the same fixture ownership checks as recorder lifecycle tests.

Run `cmake --build build/tests --config Debug --target RecorderLifecycleTests RecorderFinalizationTests`,
then `ctest --test-dir build/tests -C Debug -R 'Recorder(Lifecycle|Finalization)Tests' --output-on-failure`.
The fake filesystem does not measure real SD throughput or prove eight-track
audio continuity; those remain hardware acceptance tests.

## E282 dependent-recorder regressions

The LoopRecorder group also exercises a linear parent with a real continuous
overdub dependent, parent-first and child-first cleanup, repeated deferral, eight
dependents, and reentrant close cleanup. The production `resolveCaptureCommit`
coordinator is tested with real recorder ownership and simulated clip/history
references: yielding rollback, recursive completion, publication failure after a
dependent appears, and successful publication with immediate overdub preserved.
Additional cases cover dirty-cache write fairness and backing-underrun diagnosis.
Fixtures check that Sample, cluster and allocation counts return to zero.

The recorder trace includes source Sample identity/reference count and cleanup
phase. `SOURCE_RETAINED`, `CLEANUP_DEFERRED` (once per recorder) and `CACHE_UNDERRUN`
identify dependencies and distinguish the initiating failure from cleanup.

Validation on 2026-09-09: UnitTests, RecorderLifecycleTests and
RecorderFinalizationTests pass with UBSan (`halt_on_error=1`). The separate
address+undefined sanitizer build compiles, but ASan exits with illegal instruction
before tests start on this host. An empty `main` compiled with the same Xcode
compiler, SDK and sanitizers also exits 132, inside and outside the sandbox.
ASan validation remains outstanding on a working sanitizer host.

## Overdub storage and recovery follow-up

Build `LoopStorageTests` alongside the recorder and engine suites. It links the
production FatFS implementation to a memory-backed FAT16 disk and exercises the
shared `writeLoopSnapshot` transaction. Partial sectors must be synced before
raw cluster readers use their SD address; the regression demonstrates that a
successful FatFS write alone does not provide that guarantee.

The recorder trace's `storageFailure` records the first operation stage, raw
FatFS result, file offset and requested/actual count. `inputPeak` and
`capturedPeak` are unsigned Q31 magnitudes (the latter before 24-bit truncation).
Dirty/in-flight cache masks and input/backing flags describe the failure state.
Inspect the STORAGE_FAILED entry before later cleanup events overwrite the ring.
No file logging or allocation occurs in capture diagnostics.

Run:

```sh
cmake --build build/tests --config Debug --target UnitTests RecorderLifecycleTests RecorderFinalizationTests LoopStorageTests
UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/tests -C Debug -R '^(UnitTests|RecorderLifecycleTests|RecorderFinalizationTests|LoopStorageTests)$' --output-on-failure
```

Only ff.c's pre-existing unaligned native loads are excluded from alignment
instrumentation in LoopStorageTests; other UBSan checks remain active. This
harness does not emulate SD latency/DMA, full clip rendering or microphone input.

Finish-cache regressions additionally cover playback advancing beyond a stationary
writer's prefetch window, and background copying reaching a destination while all
eight cache entries are dirty. Both previously latched backing unavailable. The
fixed behavior preserves playback prefetch and waits without advancing capture
until destination buffers are available; subsequent card service makes progress.

Repeated-overdub regressions also exercise 100 successive parent/child recorder
handoffs, cumulative audio during delayed publication, final source-reason
release, and background copy waiting for an asynchronously loaded backing
cluster. Retired Samples are reclaimed explicitly in the fixture because the
AudioFileManager eviction service is not linked. Real String/revision allocator
coverage is in the separate Linux 32-bit `LoopRevisionOwnershipTests` target.
