# Final fix wave — checkpoint report

This is an in-progress checkpoint for the single final fix wave. It is **not** a final whole-wave verification report and makes no final-review-passed claim.

## Inherited progress (not re-reviewed or represented as new evidence)

Wave base: `2517e57c688fe8c976dbbf422d45902af4730e74`.

| Commit | Inherited subject | Review finding |
|---|---|---|
| `3a16ea67` | fix: retry loop Undo commit after its candidate seals | I1 |
| `4921f991` | fix: fence loop Redo across an evicted noninitial boundary | I2 |
| `db362372` | fix: invalidate loop Redo when a new initial take is admitted | I3 |
| `c5287be9` | fix: publish completed loop saves independently of the RAM source | I6 |
| `6444ba15` | fix: keep accepted loop checkpoint loads across a view exit | I8 |

Those commits are progress, not independent review closure. The prior host run reported 505 tests, but this checkpoint does not treat it as evidence for the new M1/M2/I5 behavior. Final whole-wave reruns remain required.

## This checkpoint slice: M1, M2, I5

### M1 — exhausted navigation epochs

- `epochs.h`: `nextEpoch`, `nextEpoch32`, and `navigationEpochsAvailable` saturate to explicit sentinels; the pure policy helper is tested at ordinary and near-maximum values without mutable production test state.
- `Engine::advanceHistoryEpoch` and `SongModel::advanceTransportEpoch` use saturating progression. `SongModel::historyTarget` returns `Unavailable` when either target epoch is exhausted; `historyTargetCurrent` rejects it thereafter. The saturated 32-bit stream generation triggers transpose reset on every adapter window, so saturation cannot suppress a required stream reset.
- Test: `LoopEpochs.exhaustedNavigationEpochsCannotAuthorizeTargets`.

### M2 — explicit TransportStop invalidation

- `SongModel::dispatch(TransportStop)` advances the transport identity even when already stopped or when it cancels queued transport work.
- Test: `LoopBindings.DirectTransportStopInvalidatesArmedTarget` (existing in-progress regression; verified in the slice run).

### I5 — capture change independent of optional History descriptors

- `ResidentAudio::captureHasWrittenFrames` tracks actual overdub writes per slot, reset only when a new overdub gate opens. `recordedInitialFrames` exposes the nonzero active-initial count. `SongModel` exposes read-only accessors for these production observations.
- `History::canAdmitInitialBoundary` checks fixed descriptor/identity capacity without reservation. `historyTarget` and `requestHistoryNavigation` reject a nonzero initial take before closure when no descriptor can be admitted. Zero-frame initial closure remains allowed.
- Model eligibility and confirmation use actual captured writes, not `History::activeSessionChanged`; a changed but untracked overdub with incomplete/missing history stays open with `SourceUnavailable`. An unchanged untracked gate may close but cannot fall through to an older boundary.
- Regressions: `LoopBindings.DescriptorExhaustedChangedOverdubRemainsRunningAndUnavailable`, `LoopBindings.DescriptorExhaustedUnchangedOverdubClosesWithoutFallingThrough`, and `LoopBindings.DescriptorExhaustionRejectsNonzeroInitialUndoBeforeClosing`. Each fills the real fixed descriptor pool through capture sessions.

## Slice TDD and verification evidence

- RED command, changed overdub: `build/tests/loop_station/LoopStationTests -t LoopBindings.DescriptorExhaustedChangedOverdubRemainsRunningAndUnavailable` while the model still used `History::activeSessionChanged` as the sole signal → failed at `CHECK_FALSE(f.model->canUndo(0))` (actual writes were advertised as undoable). Production code was restored to the ResidentAudio signal before proceeding.
- RED command, initial shortage: `build/tests/loop_station/LoopStationTests -t LoopBindings.DescriptorExhaustionRejectsNonzeroInitialUndoBeforeClosing` with the `historyTarget` availability preflight temporarily removed → expected status `4` (`SourceUnavailable`), got `0` (`Ok`). The preflight was restored before proceeding.
- Focused GREEN command: `build/tests/loop_station/LoopStationTests -t LoopEpochs.exhaustedNavigationEpochsCannotAuthorizeTargets -t LoopBindings.DescriptorExhaustedChangedOverdubRemainsRunningAndUnavailable -t LoopBindings.DescriptorExhaustedUnchangedOverdubClosesWithoutFallingThrough -t LoopBindings.DescriptorExhaustionRejectsNonzeroInitialUndoBeforeClosing -t LoopBindings.DirectTransportStopInvalidatesArmedTarget` → `OK (509 tests, 5 ran, 714 checks, 0 ignored, 504 filtered out, 12 ms)`.
- Build command: `PATH="$PWD/toolchain/current/python/bin:$PWD/toolchain/current/cmake/bin:$PWD/toolchain/current/ninja-build/bin:$PWD/toolchain/current/clang-format:$PATH" cmake --build build/tests --target LoopStationTests LoopPageAllocationGuard LoopAudioAllocationGuard LoopGestureAllocationGuard LoopHistorySnapshotAllocationGuard -j4` → all requested targets built.
- Final post-format CTest command: `ctest --test-dir build/tests --output-on-failure -R '^(LoopStationTests|LoopPageAllocationGuard|LoopAudioAllocationGuard|LoopGestureAllocationGuard|LoopHistorySnapshotAllocationGuard)$'` → 5/5 passed; LoopStationTests 4.69 s, four allocation guards passed, 5.30 s total. The focused command was rerun afterward and again returned `OK (509 tests, 5 ran, 714 checks, 0 ignored, 504 filtered out, 9 ms)`.
- `git diff --check` passed. The direct bundled clang-format dry-run entry was not executable, but the intact pre-commit clang-format hook formatted the one affected test macro on its first attempt and passed on the retry; no hook was bypassed or changed.
- Full CTest, firmware builds, and final artifact/hash are pending later slices/final wave verification.

## Remaining wave scope

I4, I7 and practical M3–M7 are not completed by this checkpoint. Other than the inherited progress table above, no finding is declared closed based solely on earlier commit subjects. Hardware acceptance remains NOT_RUN.

## Commits

Slice commit: `7402130e` (`fix: track unrecorded loop capture changes`). The user-staged `.gitignore` remains staged and was excluded with `git commit --only`.
