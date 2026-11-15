# Loop Station Undo/Redo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Confirmed selected-track layer Undo/Redo, including initial-recording removal and Undo during active capture.

**Architecture:** Extend the existing bounded History chain with initial-to-empty transitions and retain removed audio in PagePool. SongModel owns confirmed navigation, capture closure, foreground progress, and identity validation; LoopStationView owns the two-press interaction and feedback. Preserve the existing shared/native input policy and save snapshots.

**Tech Stack:** C++23, CppUTest, production-source UI probes, CMake/CTest, Python probe generators, DBT ARM firmware toolchain. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-11-15-loop-station-undo-redo-design.md` (approved).

## Global Constraints

- BACK/UNDO requests Undo on the selected track; SHIFT + BACK/UNDO requests Redo. A second matching press, separated by a release, confirms each step.
- Undo removes the latest overdub, or the initial recording when there are no remaining overdubs. One open capture session is one layer, regardless of loop passes.
- Only confirmed Undo closes active capture. The first press does not interrupt recording. Redo while capturing is unavailable.
- History remains bounded. Missing/incomplete history fences navigation: do not skip a lost overdub and remove an older layer or the initial recording.
- Initial Redo restores stopped; ordinary overdub navigation preserves playing/stopped state and phase modulo phrase length.
- A loaded checkpoint is one base recording in a new volatile chain. No persistent undo history or schema migration is added.
- Track identity, binding, output, FX, and settings survive Undo to empty. Explicit Clear still discards history.
- No SD work, allocation, phrase-length scans, or unbounded copies may be added to the audio callback. Publication uses the existing serialized boundary; reconstruction is bounded foreground work.
- Modifiers alone do not cancel confirmation; unrelated actions or target-identity changes do. No confirmation timeout.
- Never drop an accepted restore on commit Busy, publish stale work, claim failure as success, or silently remove an older layer after an unchanged active capture.
- Preserve unrelated user work, specifically the pre-staged `.gitignore` change. No merge or push.

## Review Focus

- Slot/Song memory-address reuse after a confirmation: an old prompt must never edit a newly bound recording (Task 4 identity tests and Task 5 UI tests).
- Capture completion or provisional expiry between presses: ordinary incoming frames preserve confirmation, but a completed/replaced capture cancels it (Tasks 2, 4, 5).
- Page/descriptor exhaustion while an initial recording is retained: capture admission must remain honest, existing live tracks stay intact, and retired references cannot alias a new phrase (Tasks 1, 3).
- SAVE or LOAD while a history job or immutable snapshot exists: no resurrection, false saved revision, leaked retained audio, or post-replacement publication (Tasks 4, 6).
- Reentrant foreground service and press/release retries around SD deferral: no duplicate commit, accidental confirmation, or cancellation of a newer request (Tasks 4, 5).

---

## Files and responsibilities

- `src/deluge/loop_station/page_pool.{h,cpp}`: detach/republish ownership without freeing or copying frames.
- `src/deluge/loop_station/gesture_rollback.{h,cpp}`, `resident_audio.{h,cpp}`, `engine.{h,cpp}`: safe capture closure, empty-state publication, capture/session identity, initial-boundary notification.
- `src/deluge/loop_station/history.{h,cpp}`: one chain containing initial and overdub boundaries, branching, bounded retention and fencing.
- Create `src/deluge/loop_station/history_navigation.h`: shared plain request/result value types, no UI or firmware dependencies.
- `src/deluge/loop_station/song_model.{h,cpp}`: model eligibility, identity checks, confirmed operations, job progress/results.
- `src/deluge/loop_station/song_model_firmware.cpp`: invoke bounded navigation service from the existing regularly scheduled Song foreground path; preserve checkpoint ownership.
- `src/deluge/gui/views/loop_station_view.{h,cpp}`: input confirmation, cancellation, display and BACK LED; remove timer ownership of history jobs.
- `src/deluge/gui/l10n/{strings.h,english.json,seven_segment.json,g_english.cpp,g_seven_segment.cpp}`: new strings through existing generation conventions if required. Do not manually diverge generated tables from their sources.
- `src/deluge/loop_station/persistence{,_firmware}.cpp`: modify only if regression tests reveal that empty-state or revision bookkeeping needs integration changes.
- Tests: existing `tests/loop_station/{page_pool,resident_audio,engine,history,restore,song_model,lifecycle,persistence}_tests.cpp` plus allocation guards; `tests/loop_station/ui/{input_tests.cpp,display_tests.cpp,fixture.h,probe_support.h,probe_support.cpp,generate_probe.py}`. Adjust OLED probe declarations/generation where production extraction requires it; never edit generated build files.
- `docs/features/loop_station.md`, `website/src/content/docs/features/loop_station.mdx`: matching user instructions.
- Create `docs/superpowers/reports/2026-11-15-loop-station-undo-redo.md`: measured verification and explicit hardware limitations.

## Execution prerequisites

The user must review this plan and select Native or Subagent-driven execution before implementation. For Subagent-driven execution, separately confirm implementation and reviewer models/reasoning levels, then verify the model actually dispatched. For final whole-branch review, recommend the strongest available model and obtain permission before using a different one. Ask before a new worktree; the current branch is `feature/loop-station-implementation`.

Use the existing `build/tests` configuration. If configuration is absent, first inspect dependency/tool availability; `cmake -S tests -B build/tests` uses FetchContent and is not permission to install missing dependencies silently. Project tools can be put on PATH per command:

```bash
export PATH="$PWD/toolchain/current/python/bin:$PWD/toolchain/current/cmake/bin:$PWD/toolchain/current/clang-format:$PATH"
```

For each task: write tests, run RED, implement, run GREEN, then commit only its explicit changed paths. Use `git commit --only -m "<task message>" -- <explicit paths>` so the already-staged `.gitignore` is excluded. Do not stage the whole repository.

Unless a step supplies a narrower command, its RED/GREEN test cycle uses `cmake --build build/tests --target LoopStationTests -j4` followed by `build/tests/loop_station/LoopStationTests`. RED must include the named new failing assertions; GREEN means zero failures. For allocation guards, build with `cmake --build build/tests --target LoopPageAllocationGuard LoopAudioAllocationGuard LoopGestureAllocationGuard LoopHistorySnapshotAllocationGuard -j4` and run `ctest --test-dir build/tests -R '^Loop(Page|Audio|Gesture|HistorySnapshot)AllocationGuard$' --output-on-failure`. Keep these commands separate so a failed build cannot be obscured by later output.

### Task 1: Retain a live phrase while its track becomes empty

**Files:** `page_pool.{h,cpp}`; `tests/loop_station/page_pool_tests.cpp`, `restore_tests.cpp`, `page_pool_allocation_guard.cpp`.

**Interfaces:**
- Consumes: `PhraseHandle`, `PagePool::sealedHandle`, `length`, `frame`, `replace`, `retire`, `beginPhrase`, and bounded retirement.
- Produces: `Status PagePool::detach(SlotId slot, PhraseHandle& retained)`. It invalidates only that slot's active mapping and retains the sealed record, returning its unchanged handle. Invalid/unsealed input cannot partially detach.
- Extends unchanged `Status PagePool::replace(PhraseHandle candidate, PhraseHandle& previous)` to accept a genuinely empty slot with no current active record and return invalid `previous`. It still rejects another track's or a stale record.

- [ ] **Step 1: Add failing ownership tests.** Add `DetachRetainsFramesWithoutFreeingPages`, `ReplaceIntoDetachedEmptyTrackPublishesOnlyCandidate`, `RecordAgainWhileOldVersionRetainedCannotAliasIt`, and `DetachFailureLeavesActiveMappingIntact`. Pin the core result:

  ```cpp
  PhraseHandle retained{};
  CHECK(Status::Ok == pool.detach(0, retained));
  CHECK_EQUAL(live.generation, retained.generation);
  CHECK_FALSE(pool.sealedHandle(0).valid());
  CHECK_EQUAL(5, pool.length(retained));
  CHECK_EQUAL(3, pool.frame(retained, 2)->l);
  CHECK_EQUAL(freeBefore, pool.freePages());
  ```

  Use the existing fixtures to create `live` with five frames and save `freeBefore`. Test all eight slots, occupied detached descriptors, retirement, and record reuse; assert other tracks' exact frames/generations remain unchanged. Extend the allocation guard to cycle detach/replace with zero post-initialize allocations.
- [ ] **Step 2: Establish RED.** Build `LoopStationTests` and `LoopPageAllocationGuard`; run `build/tests/loop_station/LoopStationTests -sg LoopRestore` and the new pool tests using their existing group. Missing detach initially proves the API gap; after adding its inert declaration/body, require behavioral failures before implementing ownership.
- [ ] **Step 3: Implement detach and empty-slot replace.** Use an invalid active-record index for empty; preserve existing `beginPhrase` free-record selection and bounded descriptor searches. Never copy page maps or free retained frames during detach/publication.
- [ ] **Step 4: Establish GREEN.** Rebuild and run full `LoopStationTests` and `LoopPageAllocationGuard`; require zero failures and exact retained data with unchanged free-page count.
- [ ] **Step 5: Commit.** `feat: retain loop phrases across empty-track transitions`, explicit Task 1 paths only.

### Task 2: Add safe capture closure and empty audio/engine transitions

**Files:** `gesture_rollback.{h,cpp}`, `resident_audio.{h,cpp}`, `engine.{h,cpp}`; corresponding rollback/resident/engine tests and allocation guards.

**Interfaces:**
- Consumes: Task 1 ownership and existing `GestureRollback::confirm`, `pending`, `provisional`, `ResidentAudio::finishInitial`, and engine deadlines.
- Produces: `Status GestureRollback::unbind(SlotId slot)` (only when no provisional/pending entries; invalidate binding/read revision without scanning frames); `Status ResidentAudio::detachForHistory(SlotId slot)` (forget audio provider/state after safe ownership detachment, increment content revision, do not reset History or retire phrase); `void Engine::phraseRemoved(SlotId slot)` (empty/stopped, invalidate stream/session identity, clear obsolete gestures/deadlines, retain selection/settings).
- Produces: `Generation Engine::historyEpoch(SlotId slot) const`, incremented on capture gate/session transitions, clear/removal, replacement and transport stop; not on every audio frame or provisional expiry within one session. Prevent wrap from authorizing an old token.
- Produces: `Status Engine::closeCaptureForHistory(SlotId slot)` (close exactly the current active capture; initial capture seals stopped without automatic Play/Dub, overdub closes while playback continues; confirm provisional additions, never centre double-tap cancel).

- [ ] **Step 1: Add failing tests.** `HistoryClosureOfInitialTakeNeverEntersAutomaticDub`, `HistoryClosureOfOverdubKeepsPlaybackAndRecordedFrames`, `PhraseRemovalPreservesSelectionAndSettings`, `OldPadReleaseCannotClearOrRestartRemovedPhrase`, `HistoryEpochTracksSessionsNotSamples`, and `UnbindWaitsForRollbackRetirement`. Assert after initial closure `hasPhrase=true`, `capture=false`, `playing=false`, correct frame count; after phrase removal all three are false and length zero. Configure REC_DUB_PLAY and Synced Start/Stop to prove history closure bypasses both. Cover zero frames and a provisional overdub before/after expiry without changing its target identity.
- [ ] **Step 2: Establish RED.** Build/run the existing engine, resident, and rollback groups in `LoopStationTests`; require behavioral failures for missing transitions after inert API scaffolding.
- [ ] **Step 3: Implement the transitions.** Keep pool publication and resident/engine publication in one caller-owned Boundary. Return Busy until rollback references are safe; use existing bounded foreground retirement, never drain an entire scratch buffer in a loop. Do not call destructive Clear or synthesize centre presses. Apply epoch changes through common transitions so automatic completion and transport cannot bypass invalidation.
- [ ] **Step 4: Establish GREEN.** Run full `LoopStationTests`, `LoopAudioAllocationGuard`, and `LoopGestureAllocationGuard`; require zero failures, including existing double-tap cancellation tests.
- [ ] **Step 5: Commit.** `feat: add safe loop history capture transitions`, explicit Task 2 paths only.

### Task 3: Extend unified history with initial and empty boundaries

**Files:** `history.{h,cpp}`, `resident_audio.cpp`; `tests/loop_station/{history_tests.cpp,restore_tests.cpp,before_image_fixture.h,history_snapshot_allocation_guard.cpp}`.

**Interfaces:**
- Consumes: Tasks 1–2 ownership/transitions, current before-image chain, `reserveRestore/serviceRestore/commitRestore`, and bounded retirement.
- Produces: `void History::recordInitial(SlotId slot, PhraseHandle handle)`; `bool History::activeSessionChanged(SlotId slot) const`; `bool History::activeSessionComplete(SlotId slot) const`. Initial boundaries fit the same fixed descriptor budget as overdubs. `ResidentAudio::completeInitial` registers exactly one nonzero successful base recording after rollback binding succeeds.
- Existing `canUndo/canRedo/requestRestore/reserveRestore/serviceRestore/commitRestore` signatures remain; they gain empty/initial support. Initial Undo uses pool detach and stores its retained after-version; initial Redo republishes it. At the SongModel commit layer, an invalid sealed handle after success means empty, not an error. Ordinary overdub restore remains before-image based.

- [ ] **Step 1: Add failing chain tests.** `InitialUndoToEmptyAndRedoRestoresExactStereoFrames`, `TwoOverdubsUndoAndRedoThroughInitialBoundary`, `OneMultiwrapGateIsOneLayer`, `IncompleteOverdubCannotFallThroughToInitial`, `UnchangedSessionCannotUndoPreviousLayer`, `NewRecordingAfterEmptyUndoInvalidatesOldForwardBranch`, and `EvictedInitialBoundaryCannotReappearAfterDescriptorReuse`. Build a five-frame stereo base, add distinct contributions in two closed sessions, then assert every frame at all six reverse/forward boundaries, not just the first sample. Reuse the existing snapshot/restore helpers; do not globally disable initial history to preserve obsolete test counts.
- [ ] **Step 2: Establish RED.** Build and run `LoopRestore`/`LoopHistory` and the allocation guard. Require failure showing initial recording is not currently undoable and empty-state Redo is rejected.
- [ ] **Step 3: Implement initial boundary kind and navigation.** Maintain one cursor, initial retained handle, branch invalidation and retirement rules. Do not treat missing/incomplete overdub history as an initial boundary. Bind revisions consistently on empty removal/republish using the existing mapping-replaced mechanism. Update existing tests whose session counts or earliest undo availability legitimately change; preserve pressure-fence assertions.
- [ ] **Step 4: Establish GREEN.** Run full `LoopStationTests` and `LoopHistorySnapshotAllocationGuard`. Add pressure cycles across all slots and ensure retained descriptors/pages remain within existing capacities and live audio is never reclaimed.
- [ ] **Step 5: Commit.** `feat: include initial recordings in loop undo history`, explicit Task 3 paths only.

### Task 4: Own confirmed navigation and progress in SongModel

**Files:** create `history_navigation.h`; modify `song_model.{h,cpp}`, `song_model_firmware.cpp`; `tests/loop_station/{song_model_tests.cpp,lifecycle_tests.cpp}` and test CMake/probe support for a production foreground call check.

**Interfaces:**
- Produces value types in `history_navigation.h`: `HistoryTarget` with `SlotId slot`, `bool redo`, and `Generation modelIdentity, slotGeneration, historyEpoch, transportEpoch`; `HistoryNavigationState { Idle, Applying, Complete, Failed }`; `HistoryNavigationResult` with `RestoreId id`, `SlotId slot`, `bool redo`, state, and `Status status`. No raw clip/model pointer is a sufficient identity. Give each constructed SongModel a distinct monotonic identity; exhaustion makes navigation unavailable rather than reusing identities. Increment the model transport epoch when `updateClock` observes a running/stopped transition so external transport changes also invalidate confirmation.
- Produces `Status SongModel::historyTarget(SlotId slot, bool redo, HistoryTarget& target) const`, `bool SongModel::historyTargetCurrent(const HistoryTarget& target) const`, `Status SongModel::requestHistoryNavigation(const HistoryTarget& target, RestoreId& id)`, `void SongModel::serviceHistoryNavigation(uint32_t maxFrames)`, and `HistoryNavigationResult SongModel::historyNavigationResult() const`.
- `historyTarget` describes eligibility, including active initial/complete overdub and empty-state Redo. Permit arming an unchanged active session: its confirmation closes capture and returns SourceUnavailable if still unchanged, never targeting an older boundary. `historyTargetCurrent` checks stable identity without treating added frames or changed free-page count as a different session. Update `canUndo/canRedo` to model eligibility. Keep existing low-level checkpoint/restore APIs for their current callers.
- The accepted navigation stores its own bounded job and terminal result. Call `serviceHistoryNavigation(128)` from `Song::serviceLoopStationArchive` even without SD progress; preserve serialized checkpoint restore ownership. Requests cannot recursively service or commit another request. Slot/model release invalidates accepted jobs.

- [ ] **Step 1: Add failing orchestration tests.** `ConfirmedActiveInitialUndoRetainsFramesThroughBoundary`, `ConfirmedActiveOverdubUndoRemovesOnlyThatSession`, `ActiveUnchangedOverdubDoesNotUndoInitial`, `FramesWithinSameCaptureDoNotInvalidateTarget`, `AutomaticCompletionCancelsOldCaptureTarget`, `ReboundSlotAndRecreatedModelRejectOldTarget`, `RestoreProgressesWithoutUiTimerAndCommitsOnce`, `CommitBusyRetainsJobAndRetries`, `ReentrantServiceDoesNotDoublePublish`, and `FailedRestoreKeepsCumulativeAudioAfterCaptureClosed`. Assert concrete flags, exact output frames, request identity, terminal status and unaffected other-track state. Use tiny/pressured arenas or existing injection boundaries for failures; spies may observe Boundary transitions but must not implement alternate navigation semantics.
- [ ] **Step 2: Establish RED.** Build/run these in the existing SongModel/lifecycle test groups. Also add a production-source extraction check showing the actual `Song::serviceLoopStationArchive` calls the new service; a test calling only the model service does not prove firmware wiring.
- [ ] **Step 3: Implement request validation and active closure.** Recheck identity under Boundary; close capture only after confirming the same target. Active initial produces a base then removes it, with no automatic dub. Active overdub must target its own just-closed boundary and never the preceding boundary when unchanged/incomplete. Wait for bounded rollback retirement before replacing backing data. If eligibility/admission fails before closure, leave capture running; if later reconstruction fails, keep accumulated audio, capture closed, and report failure. Cancel/obsolete pending pad gestures/deadlines as specified.
- [ ] **Step 4: Implement foreground completion.** Retain job on service or commit Busy; publish terminal result only after successful commit or explicit failure/cancellation. After successful History commit, route nonempty handles through audio replacement and empty handles through Task 2 detach/phraseRemoved; synchronize revisions and transpose stream state. New capture on the same track cancels the job before it can publish. Checkpoint restore and user jobs share the existing single restore admission gate and cannot double-service each other.
- [ ] **Step 5: Establish GREEN.** Run full `LoopStationTests`, all loop allocation guards and the production foreground probe. Verify leaving the root for a menu does not strand a job, and releasing a Song leaves no active provider, retained candidate, or background request.
- [ ] **Step 6: Commit.** `feat: own confirmed loop history navigation in song model`, explicit Task 4 paths only.

### Task 5: Add two-press input, persistent feedback and BACK indicator

**Files:** `loop_station_view.{h,cpp}`, localization source/generated tables if used; `tests/loop_station/ui/{input_tests.cpp,display_tests.cpp,fixture.h,probe_support.h,probe_support.cpp,generate_probe.py}`, OLED extraction support as needed.

**Interfaces:**
- Consumes Task 4 target, request, current-target and result APIs.
- Produces private `void LoopStationView::cancelHistoryConfirmation()`, `void LoopStationView::handleHistoryPress(bool redo)`, and `void LoopStationView::refreshHistoryFeedback()` with fixed-size confirmation state, saved HistoryTarget, BACK-down tracking and observed RestoreId. Existing public UI method signatures stay unchanged. No global undo action is invoked without a selected track.
- Model owns applying jobs; view state owns only a pending confirmation and observed result. Remove `pendingRestore_`/`pendingRestoreSlot_` and restore service from `timerCallback`; preserve unrelated pending-exit behavior.

- [ ] **Step 1: Add failing production input tests.** `UndoRequiresTwoReleasedBackPresses`, `RedoRequiresTwoReleasedShiftBackPresses`, `OppositeShortcutRearmsRatherThanConfirms`, `ShiftEdgesAlonePreserveRedoPrompt`, `UnrelatedActionsAndSelectionCancelPrompt`, `SharedMenusOwnBack`, `CardDeferralDoesNotCountAsAConfirmation`, `NoSelectionNeverDelegatesGlobalUndo`, and `TransportOrModelChangeCancelsPrompt`. Assert first press leaves exact audio/capture unchanged, release does not edit, repeated press without release cannot confirm, second matching press submits once, and third press starts a new confirmation rather than removing another layer. Exercise all eight track selections and AFFECT ENTIRE. During capture render more frames between presses; assert the same session still confirms, but automatic capture completion or fresh session invalidates it. Check full SHIFT routing through existing shared-button dispatch, not just a boolean set by the test.
- [ ] **Step 2: Add failing display/indicator tests.** `ConfirmationSurvivesGraphicsRefreshOnOledAndSevenSegment`, `ApplyingAndTerminalResultReflectActualModelStatus`, `BackLedFollowsSelectedOperationAvailability`, and `ReturnFromMenuCannotReviveOldConfirmation`. Check track/action/instruction presence rather than inventing a new screen layout. Require no completion message before model commit and correct Busy/no-history/no-memory feedback. Verify no prompt timeout even after a long sample-clock advance without another action.
- [ ] **Step 3: Establish RED.** Build/run `LoopStationTests -sg LoopUi` and its display group; require current one-press Undo/Shift behavior to fail the new assertions. Keep actual production view extraction; update declarations in generators/support rather than rewriting behavior in mocks.
- [ ] **Step 4: Implement routing and state transitions.** Respect existing card deferral and exclusive-mode ownership. Keep SHIFT/release edges noncanceling; opposite BACK shortcut starts a new eligible prompt. Cancel on unrelated accepted controls, pad actions, selection changes, focus loss/reentry, stale target and transport. For menu takeover without an explicit view focus-loss hook, invalidate on the outgoing action and always reset on focus regain. Revalidate the target during graphics and immediately before submission. Consume applying presses without queuing. Observe model results from regular graphics refresh, not timer-driven servicing.
- [ ] **Step 5: Implement localized feedback and LED.** Persist confirmation on both displays across normal redraw; choose concise strings that identify action/track and matching confirmation shortcut. In `updateLed`, update BACK from selected-model eligibility and SHIFT, while preserving CROSS_SCREEN_EDIT and shared mode indicators. Use existing display/string-generation conventions, refreshing only on state/result changes where appropriate.
- [ ] **Step 6: Establish GREEN.** Build `LoopStationTests LoopStationOledTests OledCanvasTests` and run them with CTest; require all old shared-control/menu/selection/FX tests and new confirmation tests to pass.
- [ ] **Step 7: Commit.** `feat: confirm loop undo and redo with a second press`, explicit Task 5 paths only.

### Task 6: Preserve save/load and snapshot correctness across empty Undo

**Files:** `song_model.cpp`, `persistence.cpp`/`persistence_firmware.cpp` only where necessary; `tests/loop_station/{persistence_tests.cpp,song_model_tests.cpp,archive_tests.cpp,restore_tests.cpp}`.

**Interfaces:**
- Consumes unified initial boundaries, navigation results, `PersistenceState`, content/saved revisions, existing immutable snapshot/archive APIs and checkpoint restore commit.
- Produces unchanged persistence schema/Save entry points; successful nonzero checkpoint restore registers exactly one `History::recordInitial` base after successful rebinding instead of leaving History empty. Saving an empty bound track omits its checkpoint without unbinding it or retiring its volatile Redo.

- [ ] **Step 1: Add failing persistence tests.** `SaveAfterUndoToEmptyDoesNotResurrectRecordingOnReload`, `SaveDoesNotDiscardVolatileInitialRedo`, `LoadedCheckpointIsOneUndoableBase`, `OldSnapshotCannotMarkLaterUndoStateSaved`, `RedoDuringSaveRetainsImmutableSnapshot`, and `LoadCancelsApplyingHistoryBeforeNewSongPublication`. Assert exact checkpoint count/slot identities, revisions and captured frames. Retain old snapshot while Undo/Redo swaps providers; assert it still reads its original frames. Reload an empty save with settings retained and require no hidden Redo; reload a nonempty save, undo/redo its single base and require exact format-appropriate audio (24-bit reload is not bit-identical RAM).
- [ ] **Step 2: Establish RED.** Build/run the existing persistence/archive/model test groups. Require the loaded checkpoint test to expose missing base history registration; distinguish regressions from already-correct empty-save behavior.
- [ ] **Step 3: Implement minimal integration fixes.** Use existing intentional-empty checkpoint omission and snapshot revision accounting. Register the new loaded base once, only after successful commit. Never persist retained Redo or change schema. Do not cancel/retire unrelated snapshot references just because the live track is now empty.
- [ ] **Step 4: Establish GREEN.** Run full `LoopStationTests` and allocation guards; require archive fault/restart, manifest, dirty-state, and new history/save tests all pass.
- [ ] **Step 5: Commit.** `fix: preserve loop persistence across undo to empty`, explicit Task 6 paths only.

### Task 7: Document, verify and review the complete behavior

**Files:** both feature guides and new verification report listed above; fix production/test files only for review findings with regression tests.

**Interfaces:** consumes Tasks 1–6; produces matching user instructions, evidence and a physical acceptance checklist, not new runtime APIs.

- [ ] **Step 1: Update both guides.** Document one capture session per layer; BACK then BACK for Undo; SHIFT+BACK twice for Redo; no rapid double-click requirement; confirmation cancellation; active capture continues until confirmation; initial Undo empties the track but preserves settings; initial Redo restores stopped; new edits discard Redo; explicit Clear is not undoable; bounded/fenced history; loaded checkpoint is a single base recording. Remove the misleading existing one-press Undo description. Keep browser demo limitations unchanged.
- [ ] **Step 2: Run complete host verification.** `cmake --build build/tests -j4`, then `ctest --test-dir build/tests --output-on-failure`, `python3 tests/loop_station/ui/selection_preview_test.py`, and `git diff --check`. Require all registered tests, loop allocation guards, production probes and OLED suites to pass. Run a separate UBSan host configuration with `-DLOOP_STATION_UBSAN=ON` using available tools/dependencies; if unavailable, record a blocker rather than claiming sanitizer coverage.
- [ ] **Step 3: Build firmware.** Run `DBT_NO_SYNC=1 ./dbt build Debug`; require compile/link success and record the emitted artifact path. Check changed APIs against firmware-only callers, not just extracted probes. Do not flash or claim hardware success.
- [ ] **Step 4: Record evidence.** Report actual RED/GREEN assertions, test counts, sanitizer/build results, allocation/ownership limits, and any blockers. Mark physical tests NOT_RUN unless actually performed: OLED/7SEG prompt persistence and BACK LED; initial/overdub capture confirmation; loop playback/no clicks/phase; history pressure; save/load; audio deadlines. Include delayed completion latency as a hardware measurement, not an assumed guarantee.
- [ ] **Step 5: Obtain final whole-branch review.** Recommend the strongest available reviewer model and respect the user's model selection. Review exact audio ownership, stale-token protection, provisional rollback, service scheduling, shared BACK routing, honest statuses and snapshot/persistence. Fix findings with failing regression tests and rerun affected tests plus final verification before claiming completion.
- [ ] **Step 6: Commit documentation and remaining reviewed fixes.** `docs: describe confirmed loop layer undo and redo`, explicit paths only. Do not merge/push. Present changed behavior and actual verification concisely.

## Self-review

- Spec coverage: Tasks 1–3 own retained initial/empty audio, chain ordering and pressure fences; Task 4 owns active capture, stale identity, job lifetime and scheduled progress; Task 5 owns exact shortcuts, cancellation, feedback and LED; Task 6 owns dirty/save/load/snapshot behavior; Task 7 owns docs and measured validation.
- Interface consistency: all model-level user requests use HistoryTarget and produce RestoreId; low-level History/ checkpoint APIs remain distinct. Empty transitions use detach/phraseRemoved, not destructive Clear or nonempty-only phraseReplaced.
- Review Focus is pinned to named tests in Tasks 1, 3, 4, 5 and 6. Source extraction checks verify firmware service wiring separately from model behavior.
- Task dependencies are sequential and share audio/history/model contracts; this is one feature, not independent parallel projects. No dependency installation, demo change, schema expansion, memory-profile increase or unrelated cleanup is planned.
