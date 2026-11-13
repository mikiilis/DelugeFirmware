# Loop Station Input Repair Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the existing Loop Station pads, Shift gestures and settings controls operate and visibly respond.

**Architecture:** Repair the performance view's foreground binding/input integration and the settings overlay's OLED invalidation. Exercise extracted, unmodified production event-handler bodies against the real SongModel/Engine and stub firmware UI/hardware dependencies, following the existing OLED probe pattern.

**Tech Stack:** C++23, CppUTest, Python source extraction, CMake host tests, DBT ARM firmware build.

**Spec:** `docs/superpowers/specs/2026-11-13-loop-station-input-repair-design.md`

## Global Constraints

- No loop-engine redesign, timing/default changes, new gestures, eager allocation of all eight tracks, or unrelated button behavior changes.
- Preserve existing lazy-binding transaction and retirement protections.
- Keep allocation outside the audio boundary and defer allocation/menu-opening pad actions when the SD routine is locked.
- Device acceptance is separate from host tests and firmware linking.

## Review Focus

- A release on a never-used track must not allocate a clip.
- Binding failure must leave the track idle, show feedback and allow retry.
- Shift press AND release must reach shared processing, including across the settings overlay.
- SD-locked presses must be retried without allocating or dispatching a gesture.
- OLED edits must request redraw, while seven-segment refresh must remain functional.

## File responsibilities

- `src/deluge/gui/views/loop_station_view.{h,cpp}`: foreground lazy binding and performance input handling.
- `src/deluge/gui/menu_item/loop_station/settings.cpp`: Shift propagation and display invalidation.
- `tests/loop_station/ui/generate_probe.py`: extract production handler/helper bodies with checked source markers; no replacement implementation of those handlers.
- `tests/loop_station/ui/probe_support.h`: minimal UI/hardware declarations; use real SongModel/Engine.
- `tests/loop_station/ui/input_tests.cpp`: host binding fixture, display instrumentation and interaction regressions.
- `tests/loop_station/CMakeLists.txt`: generate and compile the probe into LoopStationTests; source dependencies force regeneration.

### Task 1: Fresh-track interaction and Shift

**Interfaces:** Existing `SongModel::ensureBound(SlotId) -> Status`, `dispatch(Command) -> Status`, `clip(SlotId) -> AudioClip*`; unchanged public view event signatures. Add private `LoopStationView::prepareTrack(SlotId) -> bool` for consistent preparation/error feedback.

- [ ] Add the UI probe/fixture to LoopStationTests. Extract performance pad/button/select handlers and required helpers, plus settings input/lifecycle/display-refresh methods. Stub firmware dependencies, not model logic. A failed extraction must fail generation.
- [ ] Add `LoopUi` regressions: fresh centre press creates only its slot and starts capture; border and encoder selection prepare/select the intended slot; RECORD acts on the selected prepared slot; repeated use does not create another clip; unbound release creates none; preparation failure reports a popup and retries successfully; SD lock returns REMIND_ME_OUTSIDE_CARD_ROUTINE without allocation, then retry succeeds; Shift press/release returns NOT_DEALT_WITH in both views, with Shift+centre opening track settings after simulated shared Shift processing.
- [ ] Run `cmake --build build/tests --target LoopStationTests -j4` then `build/tests/loop_station/LoopStationTests -sg LoopUi`. Confirm the new behavioral assertions fail against current product code, not compile errors.
- [ ] Implement `prepareTrack` in the performance view; ensure readiness before preparing, call ensureBound outside model dispatch, show the existing track-unavailable popup on failure. Guard allocation-capable pad actions with `sdRoutineLock`. Use preparation on press/selection/RECORD paths, but not release or group controls. Preserve existing gesture/selection semantics and propagate SHIFT only to shared processing.
- [ ] Re-run LoopUi: binding/Shift tests pass; OLED tests in Task 2 may remain red if already added. Run the existing LoopBindings and LoopView groups.
- [ ] Review the diff and commit only the task files after passing verification.

### Task 2: Visible settings updates

**Interfaces:** Existing `SettingsMenu::refreshDisplay()`, `selectEncoderAction(int8_t)`, `buttonAction(Button,bool,bool)`; shared `renderUIsForOled()` requests OLED drawing, `uiNeedsRendering()` requests pads only.

- [ ] Add regressions asserting row movement plus OLED invalidation on SELECT turn, editing toggle plus invalidation on SELECT click, changed value plus invalidation while editing, copy-target entry/movement/confirmation invalidation, and seven-segment text/popup refresh. Also assert performance actions invalidate OLED status when relevant.
- [ ] Run the LoopUi group and confirm missing OLED invalidation failures on current code.
- [ ] In `SettingsMenu::refreshDisplay`, request OLED redraw rather than returning without action. Preserve the non-OLED branch. Request performance OLED refresh after relevant user actions and through the existing graphics refresh path as needed; no rendering work inside the audio callback.
- [ ] Run LoopUi and existing OLED suites, confirming both menu state and redraw requests rather than just a renderer snapshot.
- [ ] Review and commit only verified task files.

### Task 3: Verification and handoff

**Interfaces:** No new runtime interfaces; consume Tasks 1–2's tests and firmware sources.

- [ ] Run `cmake --build build/tests --target LoopStationTests OledCanvasTests LoopStationOledTests LoopAudioAllocationGuard LoopGestureAllocationGuard LoopHistorySnapshotAllocationGuard -j4`.
- [ ] Run `ctest --test-dir build/tests -R '^(LoopStationTests|OledCanvasTests|LoopStationOledTests|LoopAudioAllocationGuard|LoopGestureAllocationGuard|LoopHistorySnapshotAllocationGuard)$' --output-on-failure`; require all selected tests pass.
- [ ] Build firmware with `DBT_NO_SYNC=1 ./dbt build Release` using the existing toolchain/configuration; record blockers rather than claiming an unrun build passed.
- [ ] Run `git diff --check` and review the final input/SD/error paths. Obtain a whole-change independent review if the selected execution method calls for it; resolve actionable findings and rerun affected tests.
- [ ] Mark plan checkboxes accurately, record evidence, and provide the firmware output path and short physical acceptance checklist. Leave device acceptance explicitly pending until the user tries it.

## Plan self-review

The tasks cover each approved design change and all five review-focus conditions. Production handler extraction supplements, rather than replaces, existing real-model and OLED pixel tests. Firmware compilation checks actual declarations that the host UI stubs cannot validate. No change to the model's unbound-command rejection is planned.
