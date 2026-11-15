# Loop Station Standard Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore standard Deluge controls at the Loop Station root while preserving its loop gestures and targeting effects correctly.

**Architecture:** Retain `LoopStationView : RootUI`, route non-loop buttons through `view.buttonAction`, and let declined events reach `Buttons::buttonAction`. Explicitly refresh the active modulation target using the existing Song/AudioClip model stacks. Keep the audio engine and other views unchanged.

**Tech Stack:** C++23, CppUTest, production-source UI probes, Python, CMake/CTest, DBT ARM toolchain.

**Spec:** `docs/superpowers/specs/2026-11-15-loop-station-standard-controls-design.md` (approved).

## Global Constraints

- PLAY toggles normal song transport. It is not an alias for loop Start All.
- Starting transport alone does not add a new automatic loop-group launch behavior; centres and the sidebar remain the loop launch controls.
- RECORD no longer acts as a selected loop's centre press. Bound resident clips must retain their existing protection against ordinary linear audio recording.
- Preserve the eight tiles, centre-pad recording/overdub gestures, sidebar Start All/Stop All, unshifted SELECT loop settings, SHIFT-centre track settings, loop undo and existing safe exit/Clip View navigation.
- Preserve SD-card deferral and paired press/release behavior.
- Effects continue processing through existing outputs; recording remains dry. No changes to phrase length, audio ownership, archive behavior or capture timing.
- Existing controls in other views must remain unchanged. Do not overwrite unrelated working-tree changes, including the pre-existing staged `.gitignore` edit.
- No new dependencies. Host probes verify UI boundaries; they are not physical hardware acceptance.

## Review Focus

- Button releases and held modifiers: RECORD must not toggle twice or start loop capture; SHIFT + RECORD and RECORD + PLAY must reach native handling.
- Shared modal states: Save/Load holds, MIDI learn and count-in must not accidentally trigger loop gestures or exit/navigation; previously accepted pad releases must not leave a stuck hold.
- Selection and allocation failure: no selection, deselection and failed binding must never leave effects aimed at an unrelated clip.
- Focus restoration: returning from a menu or Clip View, and toggling AFFECT ENTIRE while changing selection, must retain the correct target and refresh indicators without allocating every graphics tick.
- UI context assumptions: clip-targeted effects must use the selected backing clip, not a stale `currentClip`; root-specific sidebar controls must not become timeline zoom or Session grid editing.

---

## Files and responsibilities

- Modify `src/deluge/gui/views/loop_station_view.cpp` and `.h`: routing, shared-mode guards, effects target/context refresh.
- Modify `tests/loop_station/ui/input_tests.cpp`: replace obsolete RECORD expectations and test production handler behavior.
- Modify `tests/loop_station/ui/probe_support.h`, `.cpp`, and `fixture.h`: observable shared-handler/target boundaries and shared UI mode state. Keep actual view/model/engine logic extracted from production.
- Modify `tests/loop_station/ui/generate_probe.py` only if new method placement requires it; never edit generated sources. Update `tests/oled_canvas/probe_support.h` or its generator only if the existing OLED extraction boundary needs declarations.
- Modify `docs/features/loop_station.md` and `website/src/content/docs/features/loop_station.mdx`: explain native buttons, group controls and effects targeting.
- Create `docs/superpowers/reports/2026-11-15-loop-station-standard-controls.md`: test/build evidence and hardware checklist.

## Execution prerequisites

Review this plan and choose Native or Subagent-driven execution before product-code changes. If Subagent-driven, confirm implementation/reviewer models and reasoning levels first; do not silently reuse a previous task's choices. Verify the current feature checkout and ask before creating a separate worktree. Do not merge or push.

Use the existing `build/tests` build. If necessary, configure with `cmake -S tests -B build/tests` using existing dependencies. Prefix commands with the project's bundled tools as needed:

```bash
export PATH="$PWD/toolchain/current/python/bin:$PWD/toolchain/current/cmake/bin:$PWD/toolchain/current/clang-format:$PATH"
```

### Task 1: Restore standard event routing without changing loop gestures

**Files:** `src/deluge/gui/views/loop_station_view.cpp`; `tests/loop_station/ui/{input_tests.cpp,probe_support.h,probe_support.cpp,fixture.h}`.

**Interfaces:**
- Consumes: `ActionResult View::buttonAction(deluge::hid::Button,bool,bool)`, `currentUIMode`, `UI_MODE_NONE`, existing loop pad/undo/menu/exit handlers.
- Produces: unchanged `LoopStationView::buttonAction(Button,bool,bool)` and `padAction(int32_t,int32_t,int32_t)` signatures, with standard/native fallthrough and guarded loop-only actions.
- Test boundary: a `ProbeView view` shared-handler spy with the same `buttonAction` signature, configurable `ActionResult`, and a log of the exact button/on/card arguments. Do not implement transport or metronome behavior inside this spy and call that firmware integration.

- [ ] **Step 1: Write failing routing tests.** Replace `EncoderPreparesIntendedSlotAndRecordUsesThatSelection` with selection-only coverage plus `RecordEdgesReachNativeHandlerWithoutLoopCapture`. Replace `RecordPreparesAnUnboundSelectedTrack` with a test requiring zero allocation/capture from both RECORD edges with an engine-selected unbound slot. Replace `OnlyShiftPropagatesAndCardRoutineButtonDeferralIsPreserved` with routing/deferral assertions that no longer require all root buttons to be swallowed; preserve settings-menu expectations separately.

  Add `SharedButtonsDelegateExactEdgesAndResults`: for TAP_TEMPO, SAVE, LOAD, TEMPO_ENC and shifted SELECT_ENC, assert exact arguments reach the shared handler once and its result propagates unchanged. Test PLAY/RECORD/SHIFT edges with the spy returning NOT_DEALT_WITH: require that return, zero newly bound slots and no loop capture/group launch. Repeat with SHIFT and RECORD held, ensuring the view does not replace the native combinations. Include null `currentSong` and unavailable model cases for safe standard fallback.

- [ ] **Step 2: Add failing modal/retained-control tests.** Add `SharedModesDoNotTriggerLoopActions`: with UI_MODE_HOLDING_SAVE_BUTTON, UI_MODE_HOLDING_LOAD_BUTTON, UI_MODE_MIDI_LEARN and UI_MODE_RECORD_COUNT_IN, new tile/sidebar presses, SELECT turns, loop undo and view exit must not perform ordinary loop actions. Standard buttons still delegate, including PLAY/RECORD in MIDI-learn mode. Accept a centre press before entering a shared hold, then release it: ensure it does not leave a stuck held gesture. Add exact tests for unshifted SELECT opening global loop settings, SHIFT-centre opening track settings, unchanged sidebar StartAll/StopAll and existing Clip View/exit behavior. Keep card-lock retry tests passing and check repeated retries do not duplicate actions.

- [ ] **Step 3: Establish RED.** Run `cmake --build build/tests --target LoopStationTests -j4` and `build/tests/loop_station/LoopStationTests -sg LoopUi`. Require behavioral failures against the old swallowed/remapped events, not merely missing spy declarations. Record the failed assertions.

- [ ] **Step 4: Implement focused routing.** Include `gui/views/view.h`. Remove local PLAY/RECORD command remaps. Handle retained loop shortcuts only in their applicable idle modes; unshifted SELECT remains loop settings, shifted SELECT delegates. Replace the final blanket DEALT_WITH with `view.buttonAction(b,on,inCardRoutine)` and propagate NOT_DEALT_WITH to the native dispatcher. Preserve safe deferral before actions and avoid leaking a release from a consumed loop shortcut into an unrelated shared action. Gate new loop-only actions while shared exclusive modes are active, while safely completing releases of already accepted pad gestures. Do not delegate to `sessionView.buttonAction` or add timeline inheritance.

- [ ] **Step 5: Establish GREEN.** Rebuild and run the LoopUi group, then full `LoopStationTests`; require zero failures. Inspect `src/deluge/hid/buttons.cpp` to confirm native PLAY/RECORD press/release and modifier paths remain untouched, `PlaybackHandler::endPlayback` still invokes `requestTransportStop`, and `AudioClip::wantsToBeginLinearRecording` still rejects resident audio. Document these as source checks, not dynamic end-to-end hardware tests.

- [ ] **Step 6: Commit task-owned paths.** Run `git diff --check`; stage only changed Task 1 files and commit `fix: restore standard controls in loop station view` using `git commit --only -m "..." -- <explicit task paths>` so the pre-staged `.gitignore` is excluded.

### Task 2: Bind effects to the selected loop or song

**Files:** `src/deluge/gui/views/loop_station_view.{h,cpp}`; `tests/loop_station/ui/{input_tests.cpp,probe_support.h,probe_support.cpp,fixture.h,generate_probe.py}` as needed.

**Interfaces:**
- Consumes: Task 1 routing; `view.setActiveModControllableTimelineCounter(TimelineCounter*,bool=true)`, `Song::setCurrentClip(Clip*)`, `currentSong->affectEntire`, `SongModel::clip(SlotId)` and engine selection.
- Produces: private `void LoopStationView::updateModTarget()`; overrides `bool getAffectEntire()` and `UIModControllableContext getUIModControllableContext()` reflecting the effective selected-clip/song target. Probe declarations must mirror production signatures.
- Target rule: honor existing `currentSong->affectEntire`; when false and the selected slot has a bound clip, target that clip and make it the Song's current clip. Otherwise target the Song; if no Song, clear the active target. AFFECT ENTIRE toggles only on an idle press, never on release. Do not allocate a clip just to refresh effects.

- [ ] **Step 1: Write failing target tests.** Add a shared-view target log recording exact Song/AudioClip/null pointers. Test `EntryAndNoSelectionUseSafeSongTarget`, `EveryBorderAndEncoderSelectionTargetsItsOwnClip`, `DeselectionAndFailedBindingNeverRetainUnrelatedTarget`, `AffectEntirePressTogglesOnceAndSelectionRetainsMode`, and `FocusRegainRestoresTargetAfterMenus`. Assert currentClip identity for clip targets and context/LED refresh for song versus clip targeting. With AFFECT ENTIRE active, selecting another loop must retain the song target until toggled off, then select the latest loop's clip. Centre actions can also change engine selection: test those. Do not replace engine/model selection with test-only simulated results.

- [ ] **Step 2: Establish RED.** Build and run the new tests in `LoopStationTests -sg LoopUi`; require wrong/missing target assertions against the old code after adding inert probe declarations needed for compilation.

- [ ] **Step 3: Implement target refresh.** Define the helper and overrides in production. Invoke the helper on successful opening, focus regain, successful selection-changing pad/encoder actions, and idle AFFECT ENTIRE press. Use existing modulation APIs to refresh LEDs/knob levels; do not rewrite FX processing. Synchronize currentClip before choosing a clip target. Keep menu-only focus separate from engine selection. Do not add refresh/allocation to `graphicsRoutine`. Retain Clip View for detailed track-effect menus; SELECT remains loop settings. Preserve null/unavailable guards and existing song-wide mode on entry.

- [ ] **Step 4: Establish GREEN and probe compatibility.** Run `cmake --build build/tests --target LoopStationTests LoopStationOledTests OledCanvasTests -j4` and `ctest --test-dir build/tests -R '^(LoopStationTests|LoopStationOledTests|OledCanvasTests)$' --output-on-failure`. Require all pass, including existing settings/copy tests. Test no additional clip allocation from repeated focus/graphics refresh and verify `.h` declarations match extracted bodies.

- [ ] **Step 5: Commit task-owned paths.** Run `git diff --check`, stage only Task 2 files and commit `fix: target selected loop effects in loop station view` using `git commit --only` with explicit task paths.

### Task 3: Document and verify the complete integration

**Files:** both feature guides; new report named above.

**Interfaces:** Consumes Tasks 1–2 behaviors and existing DBT/CTest commands; produces user-facing control instructions and reproducible verification evidence, not new runtime APIs.

- [ ] **Step 1: Update both guides.** Remove the statement that physical PLAY is Start All and RECORD records the selected loop. Explain native PLAY/stop and RECORD, centre-pad loop capture/overdub, independent sidebar group controls, tap tempo/SHIFT + TAP metronome, Save/Load, selected-loop knobs and AFFECT ENTIRE song effects. Clarify that entering the view and starting transport do not automatically launch the loop group; transport stop immediately stops loops. Explain preserved SELECT/SHIFT-centre settings and detailed FX via Clip View. Keep hardware status honest.

- [ ] **Step 2: Run full verification.** Run `cmake --build build/tests -j4`, `ctest --test-dir build/tests --output-on-failure`, `python3 tests/loop_station/ui/selection_preview_test.py`, `DBT_NO_SYNC=1 ./dbt build Release`, and `git diff --check`. Require all registered tests and firmware compile/link to pass. Do not install dependencies implicitly; record tool/environment blockers. Record actual test counts and emitted BIN path, not an assumed artifact name. No new UI mockup/server is needed for a nonvisual control repair.

- [ ] **Step 3: Write verification report.** Include RED/GREEN evidence, source-check limitations, build result and hardware checks marked NOT_RUN unless actually performed: native transport/record/count-in and modifiers, metronome/tap tempo, stop during initial capture/overdub, centre/sidebar operation with stopped transport, selected-loop/song FX, Save/Load and menu return, OLED/7SEG indicators. If dynamic hardware results contradict the approved transport/group distinction, stop and discuss rather than silently add auto-launch behavior.

- [ ] **Step 4: Review and commit documentation.** Obtain the agreed final whole-change review on the explicitly confirmed model, focusing on native routing/releases, modal states and FX context. Fix findings with regression tests. Commit only both guides/report after fresh verification using `git commit --only` with those paths; do not merge/push or include the unrelated staged `.gitignore` change.

## Self-review

- Coverage: Task 1 owns shared/native routing, modal-state safety and preserved gestures; Task 2 owns selection/song FX targeting and indicators; Task 3 owns both guides, build evidence and physical acceptance checklist.
- Review Focus: held modifiers/releases and shared modes are pinned in Task 1; failed binding, deselection, focus restoration, currentClip and AFFECT ENTIRE are pinned in Task 2.
- Types: production button methods return ActionResult; effects targets are existing TimelineCounters, not new loop-engine commands. Probe stubs replace only firmware boundaries.
- Scope: no audio-engine edits, new dependencies, automatic group launching, unrelated Session behaviors or claim of hardware success. Three independently reviewable deliverables; execution mode and review models remain user decisions.
