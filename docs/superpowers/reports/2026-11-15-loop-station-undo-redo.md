# Loop Station layer undo and redo — documentation and verification

## Outcome

Documented and host-verified the approved selected-track layer undo/redo feature on
`feature/loop-station-implementation`, up to `d854facc` (Tasks 1–6). Both feature guides now describe the
confirmed two-press shortcuts, layer/empty semantics, cancellation and fenced history, and save/load
behaviour; the misleading one-press undo text is removed. No production or test code changed in this
task; no schema or memory-profile change is introduced. The final whole-branch review had not been run
when this report was written.

## Documentation changes

- `docs/features/loop_station.md`
- `website/src/content/docs/features/loop_station.mdx`

Both now state: one capture gate session is one layer over multiple passes; **BACK** then released
**BACK** confirms Undo and **SHIFT+BACK** twice confirms Redo, with no rapid-double-click requirement and
no time limit; the first press leaves active capture running and confirmation closes/removes only the
targeted take; a pre-known incomplete-history/NoMemory rejection leaves capture open while a later
failure preserves cumulative audio, leaves capture closed and reports honestly; an unchanged active
overdub closes with no older-layer fallthrough; Undo to empty keeps settings/binding/output/FX and initial
Redo restores stopped; ordinary history preserves playback/phase; new edits discard the Redo branch and
explicit Clear is not undoable; history is bounded/fenced and cannot skip a missing layer; the prompt is
canceled by unrelated controls (including tempo), pads/selection, transport or a model/target change,
while SHIFT or a release alone preserve it; an accepted job continues across a harmless UI handoff and
publishes the committed result; OLED uses persistent rows and seven-segment a persistent view-owned
scrolling message; saving an undone-empty track omits its old checkpoint and volatile Redo survives Save
without becoming durable; a loaded checkpoint is one volatile base recording with no old layers; and a
live snapshot taken before navigation stays readable through a bounded owned reference. Browser-demo
limitations are unchanged.

## Host verification (fresh)

Bundled toolchain PATH was used throughout; raw logs are in
`.superpowers/sdd/2026-11-15-loop-station-undo-redo/verification/`.

```bash
cmake --build build/tests -j4
ctest --test-dir build/tests --output-on-failure
python3 tests/loop_station/ui/selection_preview_test.py
git diff --check
```

- Host build: PASS (exit 0).
- Registered suites: **27/27 pass** (final `verification/ctest.log`: 11.70 s), including all allocation
  guards, production probes, `LoopStationTests`, `LoopStationDemoContract`, OLED suites and
  `LoopFatFsAllocationTests`. (An earlier run of the same suite logged 11.52 s.)
- `LoopStationTests`: **499 tests, 499 ran, 3,907,835 checks, 0 ignored, 0 filtered**.
- `selection_preview_test.py`: **7/7 pass**.
- `git diff --check` (including staged): clean.

Observed nonfatal warnings were not hidden and not cleaned up: the Debug firmware build logs
`-Wstack-usage` at `src/deluge/deluge.cpp:409` and the RWX LOAD-segment link warning; the Release build
logs `-Wnull-dereference` at `src/deluge/gui/menu_item/cpu/voice_usage.h:90` and the same RWX warning.

## Sanitizer verification (separate configuration)

The **final** sanitizer configuration was fully offline: every FetchContent package was pointed at an
already-present `build/tests/_deps/*-src` source and `FETCHCONTENT_FULLY_DISCONNECTED=ON` was set.
Provenance is not uniform across the whole task: the first configure attempt omitted the override for the
hyphenated `cxx-prettyprint` package, so CMake cloned that tiny header-only repository from GitHub once
before the offline reconfigure. That fetched directory was discarded and the configuration below was
redone with no further download and no installation. Only this final configuration is offline-reproducible;
the initial accidental clone is recorded here rather than hidden.

```bash
export PATH="$PWD/toolchain/current/ninja-build/bin:$PWD/toolchain/current/python/bin:$PWD/toolchain/current/cmake/bin:$PWD/toolchain/current/clang-format:$PATH"
D="$PWD/build/tests/_deps"   # existing sources already populated by the earlier build/tests configure
cmake -S tests -B build/tests-loop-ubsan -G "Ninja Multi-Config" \
  -DLOOP_STATION_UBSAN=ON -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DFETCHCONTENT_SOURCE_DIR_ARGON="$D/argon-src" \
  -DFETCHCONTENT_SOURCE_DIR_SIMDE="$D/simde-src" \
  -DFETCHCONTENT_SOURCE_DIR_CPPUTEST="$D/cpputest-src" \
  -DFETCHCONTENT_SOURCE_DIR_ETL="$D/etl-src" \
  -DFETCHCONTENT_SOURCE_DIR_CPPSPEC="$D/cppspec-src" \
  -DFETCHCONTENT_SOURCE_DIR_ARGPARSE="$D/argparse-src" \
  "-DFETCHCONTENT_SOURCE_DIR_CXX-PRETTYPRINT=$D/cxx-prettyprint-src"
cmake --build build/tests-loop-ubsan --config Debug \
  --target LoopStationTests LoopPageAllocationGuard LoopAudioAllocationGuard \
           LoopGestureAllocationGuard LoopHistorySnapshotAllocationGuard LoopFatFsAllocationTests -j4
ctest --test-dir build/tests-loop-ubsan -C Debug \
  -R 'LoopStationTests|LoopPageAllocationGuard|LoopAudioAllocationGuard|LoopGestureAllocationGuard|LoopHistorySnapshotAllocationGuard|LoopFatFsAllocationTests' \
  --output-on-failure
```

The hyphen must be kept in `-DFETCHCONTENT_SOURCE_DIR_CXX-PRETTYPRINT=...`; the underscore spelling is not
recognized and lets CMake fetch the package. Build PASS; **6/6 tests pass** (9.68 s). Instrumentation
scope (`tests/loop_station/CMakeLists.txt`): `-fsanitize=undefined -fno-sanitize-recover=all` is applied
to `LoopStationTests`, `LoopGestureAllocationGuard`, `LoopHistorySnapshotAllocationGuard` and
`LoopFatFsAllocationTests` only; `LoopPageAllocationGuard` and `LoopAudioAllocationGuard` are built and run
in the same tree but are **not** UBSan-instrumented. The instrumented `LoopStationTests` reports 499/499
tests and 3,907,835 checks with no sanitizer report.

Observed nonfatal UBSan-tree warnings: ETL's invalid git-derived version during configure
(`verification/ubsan-configure.log`), `-Wenum-compare` at `src/fatfs/ff.c:5432`, and
`-Wasm-operand-widths` at `src/deluge/util/functions.h:392,398` during the build.

## Named TDD evidence, fix rounds and bounds (from this plan's task reports)

Each task recorded a behavioral RED before implementation and a GREEN after; the names and line numbers
below are taken from `.superpowers/sdd/2026-11-15-loop-station-undo-redo/task-*-report.md` and were not
regenerated for this documentation task.

- Task 1 (427 tests, CTest 24/24): `LoopPages detachRetainsFramesWithoutFreeingPages` and
  `LoopPages replaceIntoDetachedEmptyTrackPublishesOnlyCandidate` failed
  (`page_pool_tests.cpp:264,325`), and `LoopRestore recordAgainWhileOldVersionRetainedCannotAliasIt`
  failed (`restore_tests.cpp:442`).
- Task 2 (434 tests): `LoopAudio detachForHistoryRequiresSafeOwnershipAndRetainsPhrase` and the engine
  composition tests failed. The initial Task 2 change already included
  `oldPadReleaseCannotClearOrRestartRemovedPhrase`; fix round 1 renamed the detach test to
  `detachForHistoryRequiresCallerOwnedDetachAndRetainsPhrase`.
- Task 3 (441→443, then 444 tests): `LoopHistory InitialUndoToEmptyAndRedoRestoresExactStereoFrames`
  (`history_tests.cpp:202`), `OneMultiwrapGateIsOneLayerOverMultipleWraps`,
  `UnchangedSessionCannotUndoPreviousLayer`, `NewRecordingAfterEmptyUndoInvalidatesOldForwardBranch`,
  `EvictedInitialBoundaryCannotReappearAfterDescriptorReuse` and
  `TwoOverdubsUndoAndRedoThroughInitialBoundary` failed before the change. Fix round 1 added
  `LoopRestore RedoCannotSkipAnInitialBoundaryEvictedByCrossSlotPressure` (`restore_tests.cpp:558`).
- Task 4 (456→457, then 464 tests): the focused initial-take RED was `expected <0> but was <6>` at the
  first `CHECK_EQUAL(Ok, historyTarget(...))`, and the foreground probe failed with no navigation
  service. Fix round 1 added `AcceptedNavigationSurvivesRootViewExit`,
  `AcceptedNavigationSurvivesSelection`, `ObsoleteReleaseDoesNotCancelAcceptedNavigation`,
  `ClearCancelsAcceptedNavigation`, `KnownIncompleteOverdubLeavesGateOpen`,
  `KnownNoMemoryAdmissionLeavesOverdubGateOpen` and `LaterReconstructionFailureRetainsAudioAfterClosure`.
- Task 5 (475→480, then 483 tests): `LoopUi UndoRequiresTwoReleasedBackPresses` (`input_tests.cpp:1460`),
  `LoopSettingsDisplay ConfirmationSurvivesGraphicsRefreshOnOledAndSevenSegment` (`display_tests.cpp:289`)
  and `BackLedFollowsSelectedOperationAvailability` (`display_tests.cpp:388`) failed. Fix round 1 added
  `LoopUi ApplyingIsCorrelatedWithOriginatingModel` (`:1988`), `SharedModeBackReleaseClearsDownLatch`
  (`:1921`) and `TempoStyleUnrelatedEncoderActionCancelsPrompt` (`:2021`).
- Task 6 (489→497→499 tests): `LoopBindings LoadedCheckpointIsOneUndoableBase`
  (`song_model_tests.cpp:1519`) failed for missing base history. Fix round 1 added
  `LoopSnapshots RetainedSnapshotReadsOriginalFramesAcrossInitialUndoAndRedo`,
  `RetainedSnapshotKeepsOriginalFramesAcrossOverdubUndoAndRedo`,
  `RetainedSnapshotSurvivesRetirementUntilReleaseThenReclaims`,
  `RetainedSnapshotIgnoresANewerSameSlotRecordingWrite`,
  `LoopBindings InFlightSaveSnapshotSurvivesInitialUndoAndRedo` and
  `InFlightSaveSnapshotSurvivesOverdubUndoAndRedo`. Fix round 2 added
  `LoopPages retiredRecordIsNotAPublicationCandidate` and
  `LoopSnapshots RetiredPinnedSourceIsNotAPublicationCandidate`.

Final aggregate after all rounds: `LoopStationTests` 499/499, 3,907,835 checks; registered CTest 27/27.

Concrete ownership/allocation bounds (from source, unchanged by this feature):

- `kTrackCount = 8`, `kSampleRate = 44100`, `kPageBytes = 4096`, `sizeof(Frame) = 8`.
- Configured profile `Preferred` reserves `activeAudioBytes = 33,554,432` (32 MiB); `TenSecond` is
  `28,246,016` (`config.h`). No memory-profile increase.
- `PagePool`: `kMaxPagesPerRecord = 2048`, `kDetachedRecords = kTrackCount * 2 = 16`,
  `kRecordCount = 24`.
- `History`: `kMaxSessions = 64`, `kCandidateBytes = 4 MiB`, with retirement for the same 64 sessions.
- `BeforeImages`: `kMaxPages = 1024`. `Snapshot`: `kMaxSnapshots = kTrackCount = 8`,
  `kStagingFrames = kPageBytes / sizeof(Frame) = 512`.
- `GestureRollback`: `kProvisionalSamples = 15,435` (= 350 ms at 44.1 kHz), `kCapacity = 15,435`.
- The history-snapshot allocation guard drives 100,000 boundaries; the firmware foreground service budget
  is `serviceHistoryNavigation(128)` frames per tick.

## Firmware build

`DBT_NO_SYNC=1 ./dbt build Debug` compiled all 318 steps, including every new loop-station and
`loop_station_view.cpp` object, but the **final link failed** with undefined newlib syscall stubs
(`_close`, `_fstat`, `_isatty`, `_lseek`, `_read`, `_sbrk`, `_write`). No Debug artifact exists.

This is a pre-existing, feature-independent blocker: a detached temp worktree at the pre-implementation
commit `4671c801` reproduces the identical Debug link failure (same seven symbols), and object
inspection shows no object references the syscalls directly — the pull is newlib stdio reached through
libstdc++'s exception/terminate runtime, which many baseline objects already use. The Debug and Release
links differ only by `-g` (and `-DNDEBUG` at compile), and the arm-none-eabi debug C library variant lacks
the syscall/retarget objects. No linker configuration was modified.

Supplementary `DBT_NO_SYNC=1 ./dbt build Release` links successfully, confirming the feature object set is
link-clean under the project's working configuration:

- BIN `build/Release/deluge-v1_3_0-dev+2026_11_14-43f7427a.bin`, 1,944,940 bytes, sha256
  `a641159eb072ff9fb2bb028caa9045b4b625994610baeb72185a4f94f214a757`; embedded version
  `1.3.0-dev-d854facc-dirty`; ELF text 1,874,080, data 70,832, BSS 402,700. Not flashed.

## Hardware acceptance — NOT_RUN

No hardware was available, so every check below is **NOT_RUN** and none is inferred from host probes or
the Release link. The two confirmation paths are listed separately because they differ on the device:

- **Initial-capture undo confirmation (NOT_RUN).** The first **BACK** while an initial take records leaves
  capture running; the confirming **BACK** seals the frames through the confirmation, retains them for
  Redo, and the track goes empty/stopped with no automatic Play/Dub continuation; a zero-frame take returns
  to empty with no Redo entry.
- **Overdub undo confirmation (NOT_RUN).** The first **BACK** while overdubbing leaves capture running; the
  confirming **BACK** closes exactly that gate, removes the captured overdub, keeps the backing phrase
  playing, and Redo restores the additions through the confirmation boundary; a no-sample overdub closes
  with no older-layer fallthrough.
- **Audible layer removal and restoration (NOT_RUN):** no clicks and correct phase for the unchanged phrase
  length; ordinary overdub history preserves playing/stopped state and phase; initial Redo restores
  stopped.
- **History pressure on real memory (NOT_RUN):** eviction/fencing under device memory pressure, and that
  live audio, admitted capture, rollback protection and snapshots are never reclaimed as history.
- **Save/load (NOT_RUN):** save an undone-empty track and reload (no resurrection, settings/binding kept);
  volatile Redo survives Save in RAM but is not durable; a loaded checkpoint is one base with no old layers;
  in-flight save snapshot retention.
- **Display and LED (NOT_RUN):** OLED and seven-segment prompt/result readability and ergonomics, and BACK
  LED behaviour.
- **Deadlines/latency (NOT_RUN):** real memory/DSP/callback deadlines, including **measured** delayed
  completion latency, which is a hardware measurement rather than an assumed figure.

## Outstanding acceptance

Physical Deluge verification remains **NOT_RUN**. The controller's final whole-branch review (Sol xhigh)
has not yet run; deferred Minor items from Tasks 1–6 remain for that triage.
