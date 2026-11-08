# RAM-resident Loop Station Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an eight-track RAM-resident looper whose admitted live overdubs never wait for SD storage, targeting a shared 32 MiB active phrase pool with the authorized measured ten-second fallback and bounded asynchronous history.

**Architecture:** A firmware-independent bounded audio/state library owns live frames, timing and history policy. Song/AudioClip and AudioOutput adapters connect it to the existing render engine; an independent archive service writes coherent WAV checkpoints. Hardware feasibility gates the complete performance UI and production acceptance.

**Tech Stack:** Existing C++23 firmware, ARM Cortex-A9, custom memory allocator, FatFs, CppUTest host tests, CMake/DBT, Astro documentation; no new runtime libraries.

**Spec:** `docs/superpowers/specs/2026-11-05-loop-station-design.md` (November 8, 2026 revision approved by the user). Read it completely, including the self-contained behavioral baseline. This plan incorporates that revision's filling ring, accepted controls, entry/exit shortcut, reduced memory profile and one-shot overdub behavior. Plan review and execution-method selection are still pending; no product tasks below have been executed.

## Global Constraints

- Active loops reside in RAM.
- Prefer 32 MiB of shared active audio: about 95.11 seconds total or 11.89 seconds for each of eight equal phrases. A measured reduced profile must fit eight independent ten-second stereo phrases including per-phrase page rounding; no smaller profile is authorized.
- Immediate repeated overdubbing without waiting for storage is the primary requirement.
- Undo availability may temporarily decrease when SD storage falls behind, so overdubbing can continue.
- Physical sources remain LEFT, RIGHT, STEREO and BALANCED.
- Range remains +/-24.5 semitones.
- A live phrase has a positive immutable frame length; subsequent passes update the same storage.
- No filesystem access, general allocation, unbounded queue traversal or history-depth-dependent processing in live audio.
- Device-wide enable setting defaults Off; ordinary songs/recorders remain compatible.
- No silent reduction of the active pool or rollback of RAM audio on archive error.
- Select the memory profile in foreground with no live page owners; never shrink a live pool. The reduced profile is qualified only when measured operating reserves justify it, and its capacity is displayed explicitly.
- Playback progress uses a twelve-pad clockwise filling ring from physical top-left, with dim unfilled pads and a reset at each native audio wrap.
- Unmodified CROSS-SCREEN enters from Song/Session root; CROSS-SCREEN or SONG returns. Consume paired releases and preserve clip, Arranger and menu uses of CROSS-SCREEN.
- ONE_SHOT wraps while overdubbing. A centre press ending overdub closes capture immediately and stops playback at the end of the current pass without resetting phase; at frame zero it plays one complete pass. Explicit stop/exit/transport commands keep their specified policies.
- Hardware measurements establish the supported workload; host tests are not hardware acceptance.

## Review Focus

1. Entry/exit, clip deletion or song replacement while a storage callback yields: stale callbacks must not reattach a capture or release current audio (Tasks 10, 14).
2. An edit touches the same tiny phrase many times before double-tap expiry: cancellation must restore originals despite saturation and exhausted history (Tasks 4, 7).
3. Save races with provisional input and history eviction: no discarded input or mixed-generation WAV becomes durable (Tasks 6, 11, 13).
4. Eight initial takes exhaust shared memory together: deterministic fair completion, positive lengths, no corruption of admitted phrases (Tasks 2, 3).
5. Sample-clock wrap, disabled metronome and tempo changes during fractional actions: deadlines progress and native phrase speed is unchanged (Tasks 5, 9).

---

## Execution and checkpoints

This is one feature with tightly dependent components, not independent subsystem projects. Implement in an isolated worktree at execution time. Never execute the plan merely because the design was approved: obtain the execution-method selection required by the writing-plans workflow.

Tasks 1-8 implement the first feasibility gate, with a minimal developer harness rather than the complete UI. Tasks 9-14 finish engine/storage integration. Tasks 15-18 complete product controls and acceptance. If real hardware is unavailable, finish meaningful host/firmware work for the first gate, record the missing measurement, and stop at the gate; do not mark it passed or continue to complete UI on assumptions.

Commit after each independently verified task. Keep the runtime flag Off by default throughout. The code samples below are test assertions/contracts, not complete fixtures.

## File structure

New production files under `src/deluge/loop_station/`:
- `types.h`, `config.h`: pure value types, settings, measured bounds.
- `page_pool.h/.cpp`: admitted active pages and fixed-capacity bookkeeping.
- `resident_audio.h/.cpp`: initial capture, dry cumulative mixer, playback phase.
- `gesture_rollback.h/.cpp`: protected provisional before-images.
- `history.h/.cpp`, `snapshot.h/.cpp`: bounded volatile history and coherent snapshot handles.
- `timing.h/.cpp`, `engine.h/.cpp`: future-pulse deadlines, commands and eight-slot state.
- `geometry.h/.cpp`: physical tile/ring mapping and sample-frame filling-ring arithmetic, shared by pure tests and the view.
- `memory_reservation.h/.cpp`: foreground ownership of firmware memory.
- `song_model.h/.cpp`, `audio_adapter.h/.cpp`: Song/Clip binding and real audio integration.
- `archive.h/.cpp`, `persistence.h/.cpp`: storage jobs and versioned serialized settings/manifests.
- `transpose.h/.cpp`, `diagnostics.h/.cpp`: combined pitch stage and bounded counters.

New UI files:
- `src/deluge/gui/views/loop_station_view.h/.cpp`.
- `src/deluge/gui/menu_item/loop_station/settings.h/.cpp`.

Tests: new `tests/loop_station/` with `CMakeLists.txt`, `main.cpp`, named test files below and focused mocks. Add after `tests/unit` in `tests/CMakeLists.txt` to reuse CppUTest. Do not assume existing RecorderLifecycleTests or LoopStorageTests targets. Production sources are already discovered recursively by `src/deluge/CMakeLists.txt`.

Documentation/harness:
- `tests/hardware/loop_station/README.md`, `fixture.h/.cpp`, `acceptance.csv`.
- `docs/superpowers/reports/2026-11-05-loop-station-feasibility.md`.
- `website/src/content/docs/features/loop_station.mdx`.

Existing integration files are named in their owning tasks below. Avoid unrelated restructuring.

## Shared interface vocabulary

All pure interfaces live in namespace `loop_station`. Use explicit capacity/error results, never exception-driven realtime allocation.

- `SlotId = uint8_t` (0-7), `FrameIndex = uint32_t`, `SampleClock = uint64_t`, `Generation = uint64_t`.
- `Frame { int32_t l; int32_t r; }`; static assert eight-byte layout.
- `StereoSample` conversion belongs to firmware adapters; core has no firmware-global dependencies.
- `Status`: Ok, Invalid, Busy, NoMemory, SourceUnavailable, Stale, Unavailable, StorageError.
- `PhraseHandle`, `SnapshotId`, `RestoreId`: typed handles with generation checks, invalid default.
- `Settings`, `TrackSettings`, `Command`, `SlotState`, `ClockMap`, `Deadline`: defined by Task 1/5 contracts.
- `ClockMap` is a value snapshot mapping sample time to metronome-relative musical time, with tempo segments and optional external-clock state. No borrowed mutable UI data.
- `MemoryProfile`: Preferred and TenSecond only. `size_t activeAudioBytes(MemoryProfile)` belongs to `config.h`; Preferred returns 33554432, TenSecond returns `8 * roundUp(441000 * sizeof(Frame), kPageBytes)`. For the initial 4096-byte pages the latter is 28246016 bytes (6896 pages, 26.9375 MiB). Hardware page-size changes must update the derived value and its tests together.

Do not invent a second parallel core. Firmware ownership depends on Engine/SongModel; Engine does not depend on Song, UI, SampleRecorder or filesystem headers.

## Verification commands

Task 1 creates the `LoopStationTests` target and CTest entry. Standard cycle:
```sh
./dbt test --no-run
build/tests/loop_station/Debug/LoopStationTests -g GROUP
ctest --test-dir build/tests -C Debug -R LoopStationTests --output-on-failure
```
Use the shown group name per task. Missing target/header is the expected first red result only when adding a new interface; otherwise require the intended behavioral assertion to fail.

If `build/tests` already exists when Task 1 adds its CMake subdirectory, explicitly reconfigure with `cmake -S tests -B build/tests -G "Ninja Multi-Config"` before the first build; DBT configures only a missing build directory.

Sanitizer configuration (Task 1 adds target-local `LOOP_STATION_UBSAN`; instrument production sources linked into this target as well):
```sh
cmake -S tests -B build/tests-loop-ubsan -G "Ninja Multi-Config" -DLOOP_STATION_UBSAN=ON
cmake --build build/tests-loop-ubsan --config Debug --target LoopStationTests
ctest --test-dir build/tests-loop-ubsan -C Debug -R LoopStationTests --output-on-failure
```
Respect existing host architecture/toolchain constraints; if the host cannot link the existing 32-bit test setup, use the project's supported host and report the limitation. Do not silently omit sanitizer coverage.

### Task 1: Establish settings contracts and the host test target

**Files:** Create `types.h`, `config.h`, `tests/loop_station/CMakeLists.txt`, `main.cpp`, `settings_tests.cpp`; modify `tests/CMakeLists.txt`.
**Interfaces:** Produce the shared aliases/handles/Status and MemoryProfile/activeAudioBytes contract above, `Settings defaultSettings()`, `TrackSettings defaultTrackSettings(SlotId)`, `bool validSlot(SlotId)`. Settings contain RecordAction, independent ActionSettings(mode,interval), and per-track source/monitor/duration/playback/colour/transpose. Define stable interval IDs exactly as the spec. Candidate page size is `kPageBytes = 4096`; the configured profile defaults to Preferred until Task 8 measurements justify changing it.
- [ ] Write group `LoopSettings`: `CHECK_EQUAL(33554432, activeAudioBytes(MemoryProfile::Preferred))`, `CHECK_EQUAL(28246016, activeAudioBytes(MemoryProfile::TenSecond))` at the candidate page size, `CHECK_EQUAL(44100, kSampleRate)`, `CHECK_EQUAL(8, sizeof(Frame))`; defaults are Synced interval ID 8 for both actions, REC_PLAY_DUB, LEFT, monitoring On, zero transpose and the eight specified colours. Assert slots 0/7 valid, 8/255 invalid and all twelve interval IDs round-trip menu order. Assert fallback capacity includes eight separately rounded 441000-frame phrases.
- [ ] Run the standard group cycle and confirm red before production contracts exist.
- [ ] Implement value types/defaults and a standalone CppUTest target using explicit source lists; add target-local UBSan compile/link flags under the option. No audio firmware globals in the pure target.
- [ ] Run `LoopSettings` and UBSan commands; all assertions pass and no sanitizer report.
- [ ] Commit `test: establish loop station settings and host harness`.

### Task 2: Implement bounded active-page admission

**Files:** Create `page_pool.h/.cpp`, `tests/loop_station/page_pool_tests.cpp`; extend the test target.
**Interfaces:** `Status PagePool::initialize(std::span<std::byte> storage)`; `Status beginPhrase(SlotId, FrameIndex reservedFrames)`; `Status append(SlotId, std::span<const Frame> frames, FrameIndex& accepted)`; `Status seal(SlotId, PhraseHandle&)`; `Frame* frame(PhraseHandle, FrameIndex)` (null for stale handles or out-of-range frames); `Status release(PhraseHandle)`; `uint32_t freePages() const`. Pool storage is supplied by owner; metadata is provisioned once during initialize.
- [ ] Write group `LoopPages`: eight small test slots exhaust a synthetic pool in round-robin order, accepted counts sum exactly to capacity, existing frames remain unchanged, zero-frame seal is invalid, stale handles reject, release returns all pages. Fixed-duration admission rejects before allocating any pages if capacity is insufficient. With actual profile-sized host storage, both profiles admit eight separate 441000-frame takes; a reduced-profile 12-second-per-track request cannot overcommit the pool. Existing phrases are never shortened to admit another.
- [ ] Run `LoopPages` red.
- [ ] Implement fixed pages, checked arithmetic and generation handles. Initial candidate page size is 4096 bytes/512 frames; record changes only after timing measurements. Define a shared per-render admission round so simultaneous initial takes receive equal chunks before another slot claims more.
- [ ] Run `LoopPages` plus allocation guards: append/frame access make zero general allocator calls after setup; no fragmentation/compaction operation exists.
- [ ] Commit `feat: add bounded resident loop page pool`.

### Task 3: Add native playback and continuous dry capture

**Files:** Create `resident_audio.h/.cpp`, `tests/loop_station/resident_audio_tests.cpp`.
**Interfaces:** `ResidentAudio(PagePool&)`; `Status beginInitial(SlotId, FrameIndex durationLimit)`; `Status finishInitial(SlotId, PhraseHandle&)`; `Status attach(SlotId, PhraseHandle)`; `Status start(SlotId, bool oneShot)`; `void stop(SlotId)`; `Status setCaptureGate(SlotId, bool)`; `void render(SlotId, std::span<const Frame> dryInput, std::span<Frame> dryOutput)`; `FrameIndex position(SlotId) const`; later tasks install bounded history/gesture hooks before writes.
- [ ] Write group `LoopAudio`: backing [10,20,30], input [1,2,3,4,5,6,7] yields cumulative phrase [22,27,39] after seven visits, using both channels; silence preserves content. A gate close excludes the next input sample. Verify saturation without signed overflow, empty start rejection, one-shot end, two-second clock advance without phase drift, and memory-exhaustion completion preserves every accepted frame.
- [ ] Add `one_shot_dub_wraps_until_gate_closes`: a three-frame one-shot with capture open renders eight frames without stopping; position is 2. Closing capture there renders exactly the final phrase frame and stops without capturing the next input. Closing at position 0 plays three frames then stops. Repeat with LOOP to prove it continues wrapping after gate close.
- [ ] Run `LoopAudio` red.
- [ ] Implement sequential page-span rendering with modulo-frame wraps, initial frame zero, saturating arithmetic using wider intermediates, and capacity-boundary completion events. Suspend one-shot auto-completion while capture is open; on a normal overdub gate close retain phase and rearm completion at the current pass's end. `stop()` remains an immediate override for explicit commands. Render output uses pre-addition backing; monitoring is mixed separately in Task 9 so input is not doubled.
- [ ] Run `LoopAudio` and UBSan; add long randomized reference-model comparisons and exact duration/capacity stop cases. Verify zero general allocation/storage calls in render.
- [ ] Commit `feat: add RAM playback and cumulative loop capture`.

### Task 4: Protect provisional overdubs independently of undo

**Files:** Create `gesture_rollback.h/.cpp`, `tests/loop_station/gesture_rollback_tests.cpp`; modify `resident_audio.h/.cpp`.
**Interfaces:** `Status GestureRollback::initialize(std::span<std::byte>)`; `Status begin(SlotId, SampleClock, FrameIndex phase, FrameIndex phraseFrames)`; `void beforeWrite(SlotId, FrameIndex, Frame original)`; `Status confirm(SlotId)`; `Status cancel(SlotId)`; `void service(uint32_t maxFrames)`; `bool provisional(SlotId) const`. ResidentAudio installs this mandatory hook.
- [ ] Write group `LoopGestureRollback`: saturated one-frame loops wrap thousands of times; cancellation restores the original, confirmation retains additions, all eight slots work with ordinary history disabled, and expiry occurs at exactly 15435 samples. Assert a second cancel is harmless and repeated cancel/start cannot reuse scratch that is still referenced.
- [ ] Run `LoopGestureRollback` red.
- [ ] Reserve before-images for at most 15435 distinct frames plus a documented maximum dispatch-window padding per slot. Capture each original once. Implement an atomic old-frame read mapping for cancellation plus bounded restoration work; no full-phrase copy, SD dependency or callback-time allocation. Produce a finite upper bound for scratch reuse latency and a regression covering a new capture while restoration is incomplete; additional provisional writes must preserve the effective original mapping, never reject because archival history is full.
- [ ] Run `LoopGestureRollback`; allocation guards and per-call work counters remain bounded independent of phrase length. If bounded scratch reuse cannot be demonstrated, fail feasibility and report the design issue rather than waiting for SD or weakening double tap.
- [ ] Commit `feat: isolate provisional loop gesture rollback`.

### Task 5: Implement action clock and state transitions

**Files:** Create `timing.h/.cpp`, `engine.h/.cpp`, `tests/loop_station/timing_tests.cpp`, `engine_tests.cpp`.
**Interfaces:** `Deadline nextDeadline(ActionSettings, SampleClock now, const ClockMap&)`; `Engine(ResidentAudio&, GestureRollback&)`; `Status dispatch(Command, SampleClock, const ClockMap&)`; `void service(SampleClock, const ClockMap&)`; `std::optional<SampleClock> nextActionSample(const ClockMap&) const`; `SlotState state(SlotId) const`. Command covers press/release, selection, duration, group, transport/exit, acknowledge and configuration; state includes separate capture/playing/pending/error flags.
- [ ] Write groups `LoopTiming` and `LoopEngine`: at 120 BPM, straight quarter-bar from sample 0 fires at 22050; bar fires at 88200; sixteenth at 5513 (ceil 5512.5); sixteenth-triplet at 3675. Test gestures exactly on pulses, metronome offset, every interval ID, tempo edit retiming, independent duration stop, both Record Actions and all baseline table transitions.
- [ ] Add `one_shot_record_actions_and_explicit_stop`: REC_PLAY_DUB enters one-shot playback and naturally stops after one pass; REC_DUB_PLAY enters repeating overdub and survives multiple wraps. A centre press closes input immediately, leaves phase unchanged and stops at the next native phrase boundary even with a later Synced Stop Action. Stopped starts and playing retriggers queue Start Action from zero. Double-tap stop, Stop All, view exit, capture errors and transport shutdown retain their explicit command policies, including their pending-action cancellation rules.
- [ ] Run both groups red.
- [ ] Keep exact rational pulse phase alongside Q16 integration values so triplet rounding cannot accumulate drift. Count strictly future pulses; service already-due actions before computing the next window. Extend the wrapping firmware timer to SampleClock in the adapter. Define clock behavior when transport is stopped: use a continuously advancing local tempo map, then anchor cleanly on transport start.
- [ ] Run both groups with clock wrap, no audible metronome, one-shot retrigger, stop-all during captures, cancel-only gestures, stopped release/hold and error-ack-only tests.
- [ ] Commit `feat: add loop station gesture and deadline engine`.

### Task 6: Bound volatile history and coherent snapshots

**Files:** Create `history.h/.cpp`, `snapshot.h/.cpp`, `tests/loop_station/history_tests.cpp`, `snapshot_tests.cpp`; modify `resident_audio.h/.cpp`.
**Interfaces:** `Status History::initialize(std::span<std::byte>)`; `void beginSession(SlotId, Generation)`; `void beforeWrite(SlotId, FrameIndex, Frame)`; `void endSession(SlotId)`; `bool canUndo(SlotId) const`; `Status SnapshotStore::request(SlotId, Generation, SnapshotId&)`; `Status read(SnapshotId, FrameIndex, std::span<Frame>)`; `Status cancel(SnapshotId)`; `bool valid(SnapshotId) const`; `void release(SnapshotId)`. Hooks share one before-write dispatch; mandatory gesture preservation runs first.
- [ ] Write groups `LoopHistory`/`LoopSnapshots`: tiny history capacity degrades gracefully without closing gates; never traverse across a missing undo step. A snapshot of [1,2,3] remains [1,2,3] while active pages become [4,5,6], or is explicitly cancelled before a mixed result can publish. Page pressure cancels snapshots and invalidates readers via generations.
- [ ] Run both groups red.
- [ ] Use bounded page before-images, fixed descriptors and cancellable snapshot pins. Candidate history reserve is 4 MiB outside active memory, subject to Task 8 measurement; no promise of a full-phrase undo. Evict oldest non-durable entries, coalesce checkpoint requests, never evict provisional rollback data. Snapshot readers copy to fixed staging before any yielding SD call; audio may invalidate the job during that call.
- [ ] Run both groups with blocked writers, all slots editing, vector-growth-equivalent descriptor wrap/reuse, stale reads and 100000 session boundaries. Assert maximum descriptors/bytes stay fixed and no expired snapshot can return publishable success.
- [ ] Commit `feat: add bounded loop history and snapshot generations`.

### Task 7: Reserve firmware RAM and add the developer harness

**Files:** Create `memory_reservation.h/.cpp`, `diagnostics.h/.cpp`, `tests/hardware/loop_station/fixture.h/.cpp`, `README.md`, `acceptance.csv`, `tests/loop_station/memory_reservation_tests.cpp`; modify `src/deluge/processing/engines/audio_engine.cpp` only to attach an explicitly debug-enabled harness.
**Interfaces:** `Status MemoryReservation::acquire(MemoryProfile requested)`; `void release()`; `MemoryProfile profile() const`; `std::span<std::byte>` accessors `activeStorage()`, `historyStorage()`, `gestureStorage()`, `stagingStorage()` with owner lifetime. The requested profile fixes `activeStorage().size()` through activeAudioBytes; acquire never silently retries another profile. `Diagnostics::snapshot()` returns a fixed `DiagnosticSnapshot` value defined in `diagnostics.h`, including selected profile, reserved bytes and the counters below. A firmware allocator adapter is injected into host tests.
- [ ] Write group `LoopMemory`: Preferred reserves exactly 33554432 active bytes, TenSecond exactly 28246016 at candidate page size. A failed Preferred request returns failure without trying TenSecond. Profile replacement with live handles fails without releasing them; partial reservation failure releases every allocation once; pages cannot be stolen; release waits for live handles; repeated enable/disable has no leak. Harness starts eight loops and toggles capture without product menus.
- [ ] Run `LoopMemory` red.
- [ ] Allocate foreground-only pinned low-speed memory using the existing GeneralMemoryAllocator allocation policy (`allocLowSpeed` creates non-stealable allocations); verify actual external address ranges. Do not carve either active profile from the small reserved external allocator or change global allocator partitioning without evidence. Keep profile requests explicit in the developer harness; Task 8 qualifies the product's configured choice. Add counters for callback cycles, page copy work, history pressure, capture/frame totals and storage invocations. Harness can disable archive service indefinitely.
- [ ] Run `LoopMemory`, full host tests and `./dbt build debug`; inspect map/size output for unintended internal-RAM placement. Make the test-mode compile guard explicit and Off.
- [ ] Commit `feat: add loop station memory reservation and feasibility harness`.

### Task 8: Record the first hardware feasibility gate

**Files:** Create `docs/superpowers/reports/2026-11-05-loop-station-feasibility.md`; update harness files, `config.h` and measured test bounds only.
**Interfaces:** Produce documented constants for page size, history/staging/provisional reserves, bounded page-copy work, scratch reuse, permitted minimum free memory and configured MemoryProfile. Preserve Preferred at 33554432 bytes and derive TenSecond from eight rounded ten-second phrases; no third or arbitrary smaller profile. A reduced configured profile requires measurements recorded in this report, not allocation failure alone.
- [ ] Extend failing host work-bound tests for simultaneous eight-track page crossings, late double taps, clock wrap and history-disabled saturated short loops; run the relevant groups red before any fixes.
- [ ] Implement only changes justified by those failures; preserve musical behavior. Prefer the 32 MiB profile; qualify TenSecond only if measured operating reserves require it, then update configured profile, derived capacities and profile tests together. Never change a pool already owning live phrases.
- [ ] Run the harness on actual OLED and seven-segment hardware where available: long/short mixed phrases, all eight gates open, repeated rapid stop/start, SD removed/stalled, and background normal song tasks. Capture output audio plus callback maxima, low watermarks and discontinuities. Test DMA/input latency and absolute gain convention against physical input.
- [ ] Record hardware/firmware/card IDs, exact settings, selected profile, per-component reserve bytes, attempted 32 MiB results, duration and measurements; no mock result substitutes for a missing run. If TenSecond is needed, record why Preferred fails reserves and capture eight 441000-frame stereo phrases in the reduced pool. Gate passes only if a permitted active pool and required reserves fit, all admitted captures continue without storage, scratch rollback/reuse has a measured finite bound, and no audio deadline misses occur in the declared base workload. Failure of both profiles requires a requirement discussion, not a smaller allocation.
- [ ] Commit `docs: record loop station realtime feasibility results`. If unmeasured/failed, mark blocked measurements and stop before Task 9; present evidence and the smallest proposed requirement correction. Never mark the feature complete.

### Task 9: Integrate real audio rendering and action deadlines

**Files:** Create `audio_adapter.h/.cpp`, `tests/loop_station/audio_adapter_tests.cpp`; modify `src/deluge/processing/engines/audio_engine.h/.cpp`, `src/deluge/processing/audio_output.h/.cpp`, `src/deluge/model/clip/audio_clip.h/.cpp`.
**Interfaces:** `void AudioAdapter::finalizeWindow(SampleClock, size_t&, const ClockMap&)`; `void renderDry(SlotId, std::span<const StereoSample>, std::span<StereoSample>)`; `FrameIndex phase(SlotId) const`. A bound AudioClip delegates native-speed audio to this provider without an ordinary simultaneous sample voice.
- [ ] Write group `LoopAudioAdapter`: fractional deadlines split at their ceiling sample; due-now actions cannot produce zero-window loops; backing 100 plus input 10 is heard as 110 with monitor On and 100 with Off on the capture visit, with stored cumulative backing 110 in both cases. Test mono/right/balanced/stereo mappings.
- [ ] Run group red.
- [ ] Integrate `tickSongFinalizeWindows` for actions even when no ordinary transport clock is active. Derive input alignment from Task 8 measurements and real codec gain conventions. Route combined audible dry data before ordinary FX; keep monitoring arbitration per physical source.
- [ ] Run group plus existing tick interpolation/clock tests and full host tests; build Debug. Verify bound loops continue at native frame speed after tempo changes and with LoopStationView closed.
- [ ] Commit `feat: integrate resident loops into audio output rendering`.

### Task 10: Bind slots to Song/AudioClip lifecycle safely

**Files:** Create `song_model.h/.cpp`, `tests/loop_station/song_model_tests.cpp`; modify `src/deluge/model/song/song.h/.cpp`, `src/deluge/model/clip/audio_clip.h/.cpp`, `src/deluge/processing/audio_output.h/.cpp`.
**Interfaces:** `Status SongModel::ensureBound(SlotId)`; `Status unbind(SlotId)`; `void clipDeleted(AudioClip*)`; `void requestExit()`; `bool safeToRelease() const`; `Status copySettings(SlotId from, SlotId to)`; `Engine& engine()`. Song owns the model, not the view.
- [ ] Write group `LoopBindings`: lazy LOOP 1-8 independent output creation; clear retains name/output/FX; delete invalidates bindings; duplicate slot IDs keep first valid binding. Inject a yield during setup then exit/delete/reopen: late setup must not attach capture or reuse a deleted clip.
- [ ] Run group red.
- [ ] Implement generation-tagged transactions with audio-boundary detach/replace. Copy only source/monitor/duration/colour/playback/transpose; reject capturing/live-replacement destinations before any mutation. Guard ordinary sample reverse/stretch/replacement with explicit stop/unbind semantics.
- [ ] Run group with allocation failure at every creation step, capture errors isolated to one slot, and eight-slot simultaneous clear/rebind churn; build Debug.
- [ ] Commit `feat: add song-owned loop slot bindings`.

### Task 11: Archive stable generations without gating live audio

**Files:** Create `archive.h/.cpp`, `tests/loop_station/archive_tests.cpp`, `mocks/storage.cpp`; modify `src/deluge/processing/engines/audio_engine.cpp` foreground service integration.
**Interfaces:** Inject `ArchiveStorage` with `Status begin(SlotId, Generation)`, `Status write(std::span<const std::byte>, size_t& written)`, `Status finalize()`, `void abort()`. `Archive::request(SlotId, Generation)`; `void service(uint32_t maxBytes)`; `void cancel(SlotId)`; `ArchiveState state(SlotId) const`. FatFs adapter owns actual files and error diagnostics.
- [ ] Write group `LoopArchive`: a yielding write triggers new edits/cancellation and cannot publish; short successful writes/seeks fail; header/close/sync failures retain prior durable files and current RAM frames. Provisional snapshots defer admission until confirm/cancel. One active plus one coalesced request per slot stays bounded under 100000 requests.
- [ ] Run group red.
- [ ] Serialize coherent 24-bit WAVs from fixed staging, frame/generation checked before and after each yielding storage call. Finalize outside audio; round-robin jobs; rate-limit foreground CPU work independently of card latency. Preserve first error/byte counts and distinguish absent/full/protected media. Never touch live gates on storage failure.
- [ ] Run group and UBSan with no-card/full-card/reentrant completion/cancel/delete fault injection. Re-run audio tests with archive mock permanently blocked: audio samples and state transitions are unchanged.
- [ ] Commit `feat: archive loop checkpoints asynchronously`.

### Task 12: Restore bounded undo/redo states

**Files:** Extend `history.h/.cpp`, `song_model.h/.cpp`; create `tests/loop_station/restore_tests.cpp`.
**Interfaces:** `Status History::requestRestore(SlotId, bool redo, RestoreId&)`; `Status serviceRestore(RestoreId, uint32_t maxFrames)`; `Status commitRestore(RestoreId)`; `void cancelRestore(SlotId)`. Add `Status ArchiveStorage::readCheckpoint(SlotId, Generation, FrameIndex, std::span<Frame>)` for foreground durable reads; restore readers use resident before-images or this interface, with separately admitted staging.
- [ ] Write group `LoopRestore`: complete volatile steps undo/redo correctly; a missing intermediate step is not advertised; checkpoint loading is explicit and asynchronous. No staging capacity leaves live content unchanged. Edits cancel pending restore; stale completion cannot replace newer audio.
- [ ] Run group red.
- [ ] Prepare a complete replacement before switching provider at an audio boundary. Preserve phase modulo restored frame length; stopped starts reset to zero. New edits invalidate redo accessibility immediately while retained durable disk branches remain recoverable until replacement manifest publication.
- [ ] Run group with ongoing playback/tempo change/clear and restored different-length checkpoints; assert no SD access on the render path and unchanged data after each rejected restore.
- [ ] Commit `feat: add bounded loop history restoration`.

### Task 13: Persist settings, checkpoints and honest Save state

**Files:** Create `persistence.h/.cpp`, `tests/loop_station/persistence_tests.cpp`; modify `src/deluge/model/song/song.cpp`, `src/deluge/gui/ui/save/save_song_ui.cpp`, `src/deluge/gui/ui/load/load_song_ui.cpp`.
**Interfaces:** `Status Persistence::requestSave(Generation songGeneration)`; `void service()`; `SaveState saveState() const`; `Status loadSettings(Deserializer&)`; `void writeSettings(Serializer&)`; `Status admitLoadedPhrases(std::span<const SavedPhrase>)`. Serializer types stay in firmware layer; pure tests use a document adapter.
- [ ] Write group `LoopPersistence`: stable interval IDs/defaults, valid Immediate preserved, legacy fields ignored, corrupt/duplicate IDs safe; saved files quantize 32-bit working samples to valid 24-bit WAVs. Save during provisional input cannot publish cancelled additions; live edits after snapshot remain unsaved.
- [ ] Run group red.
- [ ] Add versioned RAM-loop schema and manifest generations; publish only after every referenced checkpoint has finalized. Reuse actual firmware save APIs, with staged manifest replacement/recovery chosen from tested FatFs guarantees. Load checks aggregate RAM first and leaves oversized material ordinary/unbound, never truncated. Do not advertise power-loss atomicity that FAT/card cannot provide.
- [ ] Run group with failures after every write/header/sync/close/manifest step and simulated restart: recover either previous complete or new complete referenced state, never incomplete files. Load a Preferred-sized song under the qualified TenSecond profile: excess material remains ordinary/unbound with a notice, no truncation or unexpected profile switch. Verify ordinary Song XML, Record Action and LOOP/ONE_SHOT round-trip; build Debug.
- [ ] Commit `feat: persist loop checkpoints and settings`.

### Task 14: Make transport, view exit and song replacement safe

**Files:** Extend `engine.h/.cpp`, `song_model.h/.cpp`, `archive.h/.cpp`; modify `src/deluge/playback/playback_handler.cpp`, `src/deluge/gui/ui/load/load_song_ui.cpp`; create `tests/loop_station/lifecycle_tests.cpp`.
**Interfaces:** `Status SongModel::requestTransportStop()`; `Status requestViewExit()`; `Status requestSongReplacement(bool discardUnsaved)`; `bool hasUnsavedAudio() const`. Load/UI adapters respect existing unsaved handling and return pending/cancelled status explicitly.
- [ ] Write group `LoopLifecycle`: transport immediately closes gates/stops playback; exit uses initial-take Stop Action but closes overdub input immediately; neither waits for SD. Song replacement without explicit discard cannot release volatile audio. Reopen preserves RAM/checkpoint jobs.
- [ ] Run group red.
- [ ] Cancel pending starts and invalidate startup generations, then retain handles until audio/foreground readers drain. Use explicit pending Save/unsaved discard UI for replacement/disable; no hidden wait for an archive job that cannot complete.
- [ ] Run group with delete/exit/disable/load during every yielding setup/archive/restore point; zero late attachments or double releases. Verify ordinary playback/recording and Arranger transport remain intact.
- [ ] Commit `feat: protect loop lifecycle across transport and song changes`.

### Task 15: Add combined transpose and qualify DSP load

**Files:** Create `transpose.h/.cpp`, `tests/loop_station/transpose_tests.cpp`; modify `src/deluge/processing/audio_output.h/.cpp`, production `src/deluge/processing/live/live_pitch_shifter.h/.cpp` only if needed for a bounded input adapter, and hardware harness/report.
**Interfaces:** `Status Transpose::prepare(SlotId)`; `void setCents(SlotId, int32_t totalCents)`; `void process(SlotId, std::span<Frame> combinedDry, bool discontinuity)`; `void release(SlotId)`. Allocation is foreground-only, independent of monitor/transposition settings.
- [ ] Write group `LoopTranspose`: unity is bit-exact bypass; input ratio at +/-2450 cents matches rounded Q24 conversion; bounds clamp/reject consistently; monitoring Off still transposes playback; capture WAVs remain dry. Ratio edits preserve active crossfade state; bypass exit/discontinuity resets.
- [ ] Run group red.
- [ ] Adapt the working production granular algorithm for an independent combined stereo stream; do not turn on the disabled experimental repitched-buffer macro. Bound state per slot and preserve callback continuity. Apply exactly once before ordinary FX.
- [ ] Run group plus captured-signal pitch/continuity checks and actual eight-track CPU measurements at unity and extreme shifts with declared FX settings. Update the supported envelope and memory report; if CPU budget fails, return with measurements before changing range/concurrency.
- [ ] Commit `feat: add combined loop playback and monitor transpose`.

### Task 16: Build runtime enablement and the pad performance view

**Files:** Create `src/deluge/gui/views/loop_station_view.h/.cpp`, `src/deluge/loop_station/geometry.h/.cpp`, `tests/loop_station/view_tests.cpp`; modify `src/deluge/gui/views/session_view.cpp`, `src/deluge/model/settings/runtime_feature_settings.h/.cpp`, `src/definitions_cxx.hpp` (UIType), `src/deluge/gui/l10n/strings.h`, `src/deluge/gui/l10n/english.json`, `src/deluge/gui/l10n/seven_segment.json`. Never edit generated `g_english.cpp`/`g_seven_segment.cpp` directly.
**Interfaces:** `bool LoopStationView::opened()`, `ActionResult padAction(int32_t x, int32_t y, int32_t velocity)`, `ActionResult buttonAction(deluge::hid::Button, bool, bool)`, `bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth], uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea = true)` override UI. Implement `renderSidebar` with the same row/image/mask arguments, `void renderOLED(deluge::hid::display::oled_canvas::Canvas&)`, `void selectEncoderAction(int8_t)` and `UIType getUIType()` using a new LOOP_STATION value. Pure `uint8_t filledBorderPads(FrameIndex position, FrameIndex phraseFrames)` belongs to geometry.h/.cpp: zero length gives 0; otherwise `1 + floor(12 * (position % phraseFrames) / phraseFrames)` with widened arithmetic. View sends Engine commands; it never performs timing actions or owns audio.
- [ ] Write group `LoopView`: top-left numbering 1-4 then 5-8, centre/border hit tests, no wrapping delete sweep, SHIFT no deletion, error ack no primary action. For a 120-frame phrase, filledBorderPads at positions 0, 9, 10, 60, 119, 120 equals 1, 1, 2, 7, 12, 1; zero phrase length gives 0, and maximum FrameIndex does not overflow intermediate arithmetic. Assert physical top-left then clockwise pad order, bright track-colour filled pads, dim unfilled pads, independent phrase lengths, and phase unchanged by tempo edits. Recording/overdubbing borders retain red/orange pulses rather than the playback fill.
- [ ] Add `cross_screen_navigation_preserves_context`: from idle Song/Session root, CROSS-SCREEN press enters once and its paired release stays inside; unmodified CROSS-SCREEN or SONG exits once. CROSS-SCREEN LED is steady On only at the loop root and destination indicators restore. Entry changes neither transport nor capture. Flag Off gives enable popup; held-pad edits and modal operations do not enter; active Arrangement in Song View rejects entry without changing playback; clip wrap-edit, Arranger auto-scroll, horizontal menu paging and Song KEYBOARD-to-Performance remain intact.
- [ ] Run group red.
- [ ] Add default-Off EnableLoopStation with existing runtime XML registry patterns; entry preflights MemoryReservation using Task 8's configured and measured profile. Show selected capacity, including an explicit reduced-capacity notice for TenSecond. Implement the CROSS-SCREEN/SONG entry/exit rules, paired-release handling and LED state above, route exits through SongModel::requestViewExit, and render the sample-frame filling ring. Keep hardware gestures and exact specified colours, root title, independent memory/unsaved/history status, top sidebar common-deadline group controls, REC actual gates and BACK genuinely available history.
- [ ] Run group, full host tests and Debug build; inspect OLED and seven-segment renders in harness, including pending destination blink with continuing old audio.
- [ ] Commit `feat: add loop station performance view`.

### Task 17: Add global/track menus and safe settings copy

**Files:** Create `src/deluge/gui/menu_item/loop_station/settings.h/.cpp`, `tests/loop_station/menu_tests.cpp`; modify `src/deluge/gui/ui/menus.cpp`, `loop_station_view.cpp` and relevant localization sources.
**Interfaces:** Menu adapters call `Engine::dispatch`, `SongModel::ensureBound`, `SongModel::copySettings`; global SELECT opens global settings, SHIFT-centre directly opens that track's settings. No parallel settings model.
- [ ] Write group `LoopMenus`: Start/Stop reveal interval only in Synced mode; menu order differs correctly from IDs; duration limit follows memory/bar tick constraints; cents limits and colours are exact. Copy leaves destination name/audio/FX/history/state unchanged and rejects busy destination atomically.
- [ ] Run group red.
- [ ] Build specified Record Action, source, monitoring, duration, playback, transpose, colour, Copy To Track and ordinary safe AudioClip menu access. Apply routing/monitor/colour changes at an audio-safe boundary; preserve selected FX target. Do not expose removed legacy fields.
- [ ] Run group with unbound copy destination creation failure, settings edits during capture, BACK navigation and seven-segment fallback; build Debug.
- [ ] Commit `feat: add loop station settings menus`.

### Task 18: Complete product acceptance and documentation

**Files:** Create `website/src/content/docs/features/loop_station.mdx`; update `tests/hardware/loop_station/README.md`, `acceptance.csv` and feasibility report.
**Interfaces:** Produce the documented supported performance envelope and reproducible acceptance records. No new product APIs.
- [ ] Add failing end-to-end regression cases for any still-uncovered acceptance behavior: both Record Actions, Save/Load with unsaved audio, source-monitor priority, rapid dependent gestures, long/short mixed eight-track looping, restart from durable reduced history. Verify intended red results before fixes.
- [ ] Implement only necessary fixes in owning components; run focused regressions and their UBSan tests.
- [ ] Run `./dbt test`, UBSan commands, `./dbt build debug`, `pnpm --dir website build` and `git diff --check`. Report unrelated existing failures separately; do not claim an unrun check.
- [ ] Execute hardware matrix on OLED/seven-segment models: eight stereo captures/overdubs, all start/stop intervals, SD stall/removal/full media, long sessions, repeated double taps, transpose extremes, transport/view/load lifecycle and archive recovery. Include ONE_SHOT multiwrap overdub and phase-preserving finish, both entry/exit buttons, filling-ring wrap and the measured memory profile's duration. Record actual loop output and high-water counters. Document preferred/qualified reduced capacity, shared allocation, reduced/coalesced undo, pending Save, 24-bit persistence and supported FX/song load honestly.
- [ ] Commit `docs: document and qualify RAM loop station performance`; request whole-branch review before integration. Do not merge/release or claim hardware acceptance with missing required measurements.

## Self-review and coverage map

- Intent/priority, constraints and approach: Tasks 1-8; gate explicitly prevents unsupported claims.
- Active shared memory, first-take limits and native audio: Tasks 2-3, 7-10.
- Provisional rollback, undo pressure, coherent archival and restoration: Tasks 4, 6, 11-13.
- Timing, gestures, phase, group controls, transport: Tasks 3-5, 9, 14, 16.
- Monitoring/dry taps/ordinary FX/transpose: Tasks 3, 9-10, 15.
- Persistence, save status, recovery and compatibility: Tasks 10-14.
- View, flags, geometry, LEDs, menus/copy: Tasks 16-17.
- Hardware/host validation and user guide: Tasks 8, 15, 18.
- November 8 memory profiles and eight-by-ten-second admission: Tasks 1-2, 7-8, 13, 16, 18.
- November 8 one-shot repeating overdub and native boundary completion: Tasks 3, 5, 13, 18.
- November 8 filling-ring arithmetic and CROSS-SCREEN/SONG/LED navigation: Task 16, then Task 18 hardware acceptance.
- All five Review Focus cases have explicit owner tests. No product requirement is delegated to visual inspection alone.
- Pure types and handles are shared; adapters own firmware types. Every newly introduced API is assigned to an owning task.
- Numerical reserve candidates outside the active pool are measurement inputs, not approved hardware results.
- Plan self-review completed against the November 8 approved design, including the revised memory profiles, phase-preserving one-shot completion, and paired-release navigation. Task 7 owns the profile-aware MemoryReservation interface, used by Task 16; no obsolete zero-argument acquire call or fixed active-span assumption remains. All 18 product tasks remain unchecked; no implementation or hardware verification has been performed while writing this plan.
