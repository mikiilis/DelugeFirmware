# Loop Station E282 investigation and fix plan

Status: software changes implemented; host regressions pass. Device validation of
the initiating failure and recovery remains outstanding (2026-09-09).

## Finding

E282 is a recorder-cleanup ownership assertion, not an SD error code. Before
this fix, `SampleRecorder::cleanupAbortedRecording()` released the recorder's
Sample ownership and then asserted that no Sample references remained before
attempting deletion.

The current continuous-overdub implementation introduces another legitimate
owner: a dependent overdub holds its source Sample through `loopSource_`.
That reference is acquired by `setupLoopOverdub()` and released by
`releaseLoopClusters()`. Calling `abort()` only changes recorder status; it does
not release that source reference immediately.

This creates a confirmed E282 failure path when an overdub starts against a
preceding take that is audible but still awaiting successful WAV finalization.
It can occur with automatic REC → DUB → PLAY, or a manual overdub started before
the preceding recording/overdub revision finishes publishing.

## Original failing sequence

1. Recorder A finishes capture. Its Sample becomes audible and is staged for
   publication; A is retained while file finalization completes.
2. Recorder B starts overdubbing A's Sample and takes an additional source
   reference. This overlap is intentional for immediate overdub response.
3. A fails finalization/publication. The view rolls the clip and staged history
   back to the last durable revision.
4. `processPendingCommits()` calls `A.releaseCommit(false)` before notifying
   the engine, which aborts the dependent capture. That release permits A's
   aborted cleanup to run.
5. B still holds A's Sample. Even if B has already been marked aborted, its
   source reference remains until B's cleanup/destruction.
6. The recorder list is processed in insertion order: A is normally before B.
   A releases its own Sample reference, sees B's remaining reference, and
   triggers E282 before B gets its cleanup turn.

Therefore, merely moving the engine notification before `releaseCommit(false)`
is insufficient. The dependent source ownership must actually drain, or parent
cleanup must safely defer deletion while retaining its own ownership.

Relevant code:

- `src/deluge/model/sample/sample_recorder_lifecycle.cpp`: abort, commit release,
  cleanup and the E282 assertion.
- `src/deluge/model/sample/sample_recorder_loop.cpp`: source retention, cache
  ownership and yielding SD operations.
- `src/deluge/gui/views/loop_station_view.cpp`: `processPendingCommits()` rollback
  ordering and capture failure detection.
- `src/deluge/processing/engines/audio_engine.cpp`: insertion-ordered recorder
  cleanup and recorder capacity accounting.

## Reproduction evidence

A temporary host harness linked the production lifecycle, card and continuous
loop recorder objects against the existing recorder-test fixtures. It created
A, retained A for commit, created B with A's Sample as its source, released A's
failed commit, aborted B, and invoked cleanup in recorder-list order.

Observed output:

```text
Before rollback: parent Sample references = 2
Parent-first cleanup: E282; remaining Sample references = 1
Live samples/clusters/allocations after test cleanup: 0/0/0
```

The control run cleaned B first:

```text
Before rollback: parent Sample references = 2
Dependent-first cleanup: parent references before deletion = 1
Live samples/clusters/allocations after test cleanup: 0/0/0
```

The harness catches the assertion as a fixture exception and restores the
already-released writer reference only to permit test teardown. That recovery
is not a firmware fix. The reproduction is in `/tmp/loop_e282_repro.cpp` for
this workspace session; permanent regressions now cover this sequence.

This proves an E282-producing ownership sequence. It does not establish which
original storage/allocation/backing failure occurred on the reported device.
Red blinking track LEDs are consistent with the engine's Error state, which
can precede the cleanup assertion. Hardware traces are still needed to identify
the first failure in the user's occurrence.

## Implementation plan

### 1. Add permanent failing regressions

Extend RecorderLifecycleTests/LoopRecorder with a retained parent and an actual
`setupLoopOverdub(..., parent.sample)` dependent recorder. Cover parent-first
cleanup, dependent-first cleanup, repeated cleanup calls, and an already-aborted
dependent. Expected fixed behavior: defer safely when needed, then drain both
recorders with no freeze, underflow, premature file deletion or leaked reasons.

Add an integration test around the real rollback coordinator with controllable
SD/allocator yields. The existing single-recorder failure tests and manual
clip/history reason-release test do not exercise dependent-recorder ownership.
An engine-only mock cannot verify this ordering.

### 2. Make aborted cleanup safe and repeatable

Separate releasing writer resources from releasing the recorder's final Sample
ownership. Track cleanup progress explicitly so retrying never calls detach
against an already-released ownership reason.

At a safe point, release the aborted recorder's cache/source dependency exactly
once. If other owners still retain its working Sample, keep the recorder's own
Sample reason and defer deletion/unlinking. Allow other recorders to make
progress; do not busy-wait on the audio or card thread. Delete the Sample/file
and release recorder capacity only after ownership has drained safely.

Preserve the existing setup/SD-operation guards. Aborting during a yielding
seek/write/close must not free data or a snapshot still in use. Do not remove
E282 or force the reference count to zero as a substitute for fixing ownership.

### 3. Coordinate rollback of dependent captures

Treat a failed parent and its dependent overdub as one rollback transaction:
close dependent input immediately, stop its use of the rejected Sample, reject
working history, restore the last durable revision, and finish deferred cleanup.

Protect the slot against reentrant gestures/completion while this transaction
is active. A normal successful commit must still allow the already-requested
immediate overdub; do not restore a musical or file-finalization wait globally.
Preserve the first meaningful error and show one slot error instead of a
secondary cleanup assertion. Do not truncate durable redo history on failure.

Audit clip deletion, song replacement and temporary-file garbage collection as
part of the same change. `deleteAnyTempRecordedSamplesFromMemory()` currently
runs one recorder pass before scanning temporary Samples; deferred cleanup must
not leave a protected Sample eligible for premature deletion there.

### 4. Identify and address the initiating error

Extend the existing bounded recorder lifecycle trace with source-Sample identity,
parent/dependent relation, cleanup phase, and source/working reference counts.
Record the first terminal failure and the point where cleanup defers. Keep trace
writes allocation-free and avoid SD logging from the audio callback.

Separately test cache misses and write fairness: the cumulative mixer currently
labels a missing/unloaded cache entry as SD_CARD, and the writer scans dirty
entries from the same beginning on every pass. These are candidates for errors
under load, not proven causes of the reported occurrence. Use targeted tests and
hardware traces before choosing prefetch/fairness changes; distinguish an
underrun from an actual failed filesystem operation.

## Acceptance matrix

| Scenario | Required result |
| --- | --- |
| Initial take fails while automatic overdub is active | Both provisional captures rejected safely; no E282 |
| Pending overdub revision fails while another overdub uses it | Last durable revision retained; dependencies drain |
| Parent cleaned before aborted dependent | Deferred cleanup, then eventual complete release |
| Failure/abort during seek, write, close or rollback reload | No freed in-flight buffers; original error retained |
| Repeated background servicing and rapid gestures | No double detach, duplicate publication or recorder leaks |
| Stop All, transport stop, view exit, clip deletion, song change | No pending owner stranded and no premature temp-file deletion |
| Successful REC → DUB and manual overdub | Immediate input behavior and cumulative audio unchanged |
| Multiple short loops and eight stereo tracks under SD pressure | Independent slot recovery, correct budget and history |

Run UnitTests, RecorderLifecycleTests and RecorderFinalizationTests with UBSan
(and ASan where supported), build debug firmware, then reproduce on hardware
with both Record Action orders, several loop lengths, multiple tracks and slow
or fault-injected SD operations. Verify the first error, recovery, audio and
history—not just the absence of E282.

Completion criteria: the permanent reproduction and acceptance tests pass,
failed working audio is never published or used after release, and successful
overdub timing remains unchanged. Device validation must confirm the initiating
failure and recovery before declaring the reported case fixed.


## Implementation record — 2026-09-09

- Split idempotent writer-resource release from final Sample detachment. Aborted
  cleanup retains its Sample reason while other owners remain, releases source
  dependencies even while its own cleanup waits, guards reentrant close, and
  unlinks only after ownership drains. E282 remains an invariant assertion.
- Added a shared production commit coordinator. Failed publication aborts the
  current dependent before rollback; the parent stays commit-retained through
  history restoration and engine notification. Reentrant completion and slot
  gestures are guarded. Successful publication preserves immediate overdub.
- Clip deletion and song destruction release pending commit holds. Temporary
  Sample garbage collection skips retained Samples. Error acknowledgement
  preserves an already-requested stop.
- Added bounded RAM diagnostics for source identity/reasons, cleanup phase,
  first deferral and cache underrun. Abort closes input immediately. Missing
  prefetched backing no longer invents an SD error, and failed capture stops
  processing further input. Dirty-cache service rotates to prevent a repeatedly
  dirtied early entry starving later entries. Background backing copy uses one
  scratch audio buffer instead of allocating a second silence buffer on stack.
- Added nine permanent recorder regressions: linear parent/dependent cleanup,
  eight dependents, child-first/repeated cleanup, reentrant close, write fairness,
  underrun classification, yielding/reentrant rollback, successful immediate
  dependent capture, and publication failure with a dependent created during a
  yield. These link the production recorder and shared commit coordinator;
  clip/history ownership and storage remain fixtures.

### Validation and limits

UnitTests, RecorderLifecycleTests and RecorderFinalizationTests pass with UBSan
and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` (3/3 suites). The final
`./dbt build debug` succeeds and produces `build/Debug/deluge.bin`; the existing
startup stack-usage and linker RWX warnings remain. `git diff --check` passes.
The ASan+UBSan recorder
build compiles but cannot run on this host: both test binaries and an empty
control program terminate with illegal instruction (exit 132) before execution.
The control also fails outside the sandbox. This is an environment validation
limitation, not an ASan pass.

Host tests cover the reproduced ownership failure and selected yielding/failure
paths. They do not simulate physical SD throughput, actual UI gesture reentrancy,
whole-song teardown, or eight-track audio performance. The acceptance matrix
above remains the hardware checklist. No device run has identified the first
failure from the reported occurrence; capture the recorder trace when testing
both Record Action orders, immediate/manual overdub, short and long phrases,
stop/exit/delete/song replacement, history recovery, and SD pressure.

## Follow-up: overdub SD failure and silence after failure

The reported device sequence was refined during investigation: built-in mic,
Source Left; normal sampling works; Loop Station first recording works after a
reboot; SD CARD ERROR appears while overdubbing. This does not establish that
subsequent WAVs contain zeros, as opposed to playback/monitoring being inaudible.
The exact device filesystem failure remains unconfirmed.

Implemented follow-up changes:

- The production snapshot writer now checks the actual seek position and byte
  count. Successful but truncated seeks/writes report card full. Raw filesystem
  errors and operation stage (seek/write/sync/close) are retained, with offset,
  requested/actual byte count, input/captured peaks and cache masks in the bounded
  RAM recorder trace. Non-storage allocation failures no longer set hadCardError.
- Production FatFS over an in-memory FAT16 disk reproduces a partial-sector
  coherency defect: write succeeds while the final bytes remain only in FIL.buf.
  Raw cluster reads can therefore see stale data. Partial-sector snapshot writes
  now sync before publishing an SD address or allowing clean-cache eviction.
  This is a proven data-integrity fix, not proof of the reported SD error's cause.
- A failed loop writer immediately aborts/gates input. AudioClip abort detaches
  the rejected output recorder before background resource cleanup. Active engine
  failure handling is guarded against recursive failure, pad actions and Stop
  All; nested commit/capture guards preserve their enclosing state.
- Monitoring eligibility is unchanged: capturing tracks have priority, followed
  by the selected idle track. Error/state callbacks and background servicing
  already recompute it. An unselected failed track losing its monitoring claim
  is expected; no automatic selection or global mic-gain change is introduced.

`LoopStorageTests` links production FatFS and the same snapshot transaction used
by the recorder. It covers repeated rewrites spanning twelve filesystem clusters,
partial-tail persistence and raw rereads, disk-full short seek, write failure,
sync failure and successful new-file creation after failure. Recorder tests
check immediate abort and preserved filesystem evidence. Engine regression
injects recursive failure/gestures during abort and verifies acknowledge/retry.
The new storage test uses UBSan with alignment instrumentation disabled only for
ff.c, whose existing native little-endian loads intentionally use unaligned
addresses. Remaining sanitizer instrumentation stays enabled.

Hardware completion gate: run the same sequence on the final binary, inspect
`recorderLifecycleTrace` at STORAGE_FAILED (including storageFailure), compare
saved WAV peaks/content outside Loop Station, verify selected-track monitoring,
then acknowledge and retry without rebooting. Repeat with Monitoring Off, stop
requests, multi-track SD load and phrase lengths above the cache capacity.
The in-memory disk does not model physical SD timing, DMA or audio rendering.

Follow-up validation: UnitTests, RecorderLifecycleTests, RecorderFinalizationTests
and LoopStorageTests pass (4/4 suites) with UBSan, subject to the ff.c alignment
exception above. The final debug firmware build and `git diff --check` pass.
Existing startup stack-usage and linker RWX warnings remain. Device acceptance
is still outstanding.

Firmware: `build/Debug/deluge.bin`
SHA-256: `5d35b6b971e4f40229d8580dbc8ea43c4d3958e3593bef9d1067bf510a79b5c3`

## Follow-up: backing unavailable when ending overdub

Two permanent recorder regressions reproduce the reported error category in the
previous implementation:

1. After input closes, playback advances independently of the background writer.
   Prefetch followed only the writer, so playback could request a cluster outside
   that window and latch CACHE_UNDERRUN/BACKING_UNAVAILABLE. This also happened
   while a fully captured revision waited to be committed: its writer cursor no
   longer moved, but playback continued.
2. When dirty entries filled the cache, prefetch deferred allocating the next
   destination. Background copying nevertheless attempted to write there and
   latched the same terminal backing error, despite having no live input to lose.

Finishing now prefetches four clusters for audible playback and two for the
background copy cursor, deduplicated within the existing eight-entry cache.
Dirty and in-flight entries remain pinned. A copy batch checks all destination
clusters before reading/copying source audio; missing or loading destinations
leave the cursor and captured count unchanged until card service can make room.
Active-input underruns still reject the take rather than silently dropping input.
No fallback to old audio, new timing setting or larger cache is introduced.

The two regressions fail before this change and pass afterwards. All four host
suites pass with UBSan and the previously documented FatFS alignment exception.
Device confirmation is still needed for the exact occurrence, particularly long
phrases, an early overdub stop, ongoing playback and a preceding pending commit.

Backing-finish fix: debug firmware build passes; existing linker RWX warning
remains. Binary SHA-256: `d59ba52e6f1477f96f6e2cceac617dd3c275a75d79f69a7c36892d8906a4d48c`.

## Repeated overdub / S001 follow-up (2026-09-21)

Implemented the approved ownership and reentrancy work:

- Capture startup now holds the slot's transaction guard through preflight,
  allocation, recorder attachment and the final state notification. A yielding
  callback cannot start a second recorder or clear the same slot. Other slots
  remain usable.
- Exit readiness includes active transactions and deferred transport stops.
  An exit requested during startup drains the resulting capture. Transport stop
  intent survives startup and automatic REC-to-DUB handoff.
- Reopening the view preserves pending recorder references and active engine
  state. Load/clear-song gestures during capture request a drained exit first;
  press Load again from Song View once draining finishes.
- Revision ownership/history methods moved from song.cpp into
  `src/deluge/loop_station/revision.{h,cpp}` without changing the WAV/history
  format or Sample reason contract.
- S001 now records `deluge::loop_station::lastStringFault` in RAM before reading
  the suspect allocation's metadata: destination/source String addresses,
  backing pointer, caller, slot, operation, Song, clip and recorder pointers.
  The existing recorder lifecycle ring retains source reasons and storage/cache
  failures. These diagnostics do not write to the SD card.

An additional source-level defect was found in S001's address validation. The
old lower bound was `EXTERNAL_MEMORY_END - RESERVED_EXTERNAL_ALLOCATOR`, but the
actual general external allocator starts a further
`RESERVED_EXTERNAL_SMALL_ALLOCATOR` (128 KiB) below that address. Valid Strings
in that part of the general arena could therefore freeze with S001. The check
now validates against both real external region boundaries. The assertion is
still present; invalid pointers are not accepted. This is a confirmed defect,
but the user's specific S001 occurrence has not been captured on hardware.

Validation adds startup/preflight reentrancy, busy-exit, transport-stop and 100
successive delayed engine commits; 100 recorder handoffs check cumulative audio
and drained source reasons. An asynchronous backing-read test verifies that
background copy waits without reporting an underrun. The fixture explicitly
reclaims unreferenced Samples because it has no AudioFileManager eviction pass.

`LoopRevisionOwnershipTests` is a new Linux 32-bit CTest target. It compiles the
real revision code, String, vector allocator and memory-region implementation;
only audio/SD boundaries and host arena setup are fixtures. It covers 101
publications with vector growth, path changes after staging, rollback, redo
branch replacement, copy/move/self-assignment and general-arena String cloning.
It must run on the existing multilib Linux test runner. Local ARM 32-bit syntax
checks are supplementary and do not constitute execution of this suite.

Device acceptance remains: repeat record/play/overdub at least 100 times, with
short and long loops, early stop, pending publication, undo/redo, exit/reopen,
transport stop and injected SD failures. Verify cumulative audio and monitoring,
that failed revisions preserve durable audio, and that all slots recover without
reboot. Capture the String fault snapshot and recorder lifecycle ring if any
S001/backing error remains.

Final local validation (2026-09-21): UnitTests, RecorderLifecycleTests,
RecorderFinalizationTests and LoopStorageTests all pass with
`UBSAN_OPTIONS=halt_on_error=1` (the existing FatFS-only alignment exception
remains). The ARM 32-bit compiler accepts the ownership test, revision module,
String and allocator fixture configuration. `./dbt build debug` succeeds and
`git diff --check` is clean. Linux execution of LoopRevisionOwnershipTests and
hardware acceptance remain outstanding. Debug binary SHA-256:
`54a36dc4071dcc5881d9f6a5fd1097f768623248fa61210ee13f0b747f7fe281`.
