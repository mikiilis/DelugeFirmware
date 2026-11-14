# Loop Station Shared Settings UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give global and per-track Loop Station settings the existing four-column, instrument-button-selected horizontal UI, including semitone/SHIFT-cent transpose editing and an identity-colour pulse identifying the selected or settings-focused track.

**Architecture:** Retain `SettingsMenu` as the Loop Station UI host and delegate horizontal presentation/navigation to `HorizontalMenu`, with narrow virtual hooks replacing sound-editor assumptions for external hosts. Bind Loop Station-specific native selection, integer and decimal menu items to the existing configuration commands; retain the current list as the feature-off/seven-segment fallback. Copy review remains a deliberate host-owned action.

**Tech Stack:** C++23, CppUTest, CMake/CTest, Python production-method probes, real OLED canvas/fonts, bundled DBT ARM toolchain, existing static HTML documentation demo. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-11-14-loop-station-settings-ui-design.md` (approved November 14, 2026, then amended with the user's focused-ring pulse requirement).

**Current scope ruling:** The November 14 user ruling in Task 4 supersedes historical pulse references in the goal, constraints, files and review focus below. Ring pulse/focus override/redraw scheduling is outside this plan for separate future planning; it is not implemented or accepted here. Device acceptance remains NOT_RUN until observed.

## Execution evidence (November 14, 2026)

- Tasks 1–3: implemented and independently reviewed, including permission-isolation and SELECT-release fixes (`4a7d1e5e`, `62d7ff5d`, `43f7427a`, `373952e8`, `2ebbf0b5`). Historical step details/RED observations are retained below and in the task reports/ledger.
- Task 4: OUT OF SCOPE by user ruling; unchecked steps are deferred work, not blockers for the amended scope.
- Task 5: implemented at `ad68c6a7`; report records real rendering/fallback GREEN and self-review. Browser/device observation NOT_RUN.
- Task 6: fresh requested six-target build PASS; CTest 20/20; LoopStationTests 383 tests / 3,900,216 checks; exact DBT Release build PASS. Final independent whole-branch review remains pending with the parent session (Step 3 unchecked).
- Final verification/BIN identity, warnings, coverage limits and unchecked device checklist: `docs/superpowers/reports/2026-11-14-loop-station-settings-ui.md`. No hardware or pulse acceptance is implied.

## Global Constraints

- Apply this to **both per-track and global Loop Station settings**.
- **Reuse shared horizontal-menu components**, not a separate implementation of the same UI.
- No changes to recording/overdub timing, transport, pitch processing, audio allocation, persistence schema or track defaults.
- Retain integer-cent storage and range **-2450 to +2450 cents**, inclusive; display semitones with two decimal places.
- An ordinary encoder step adds/subtracts **100 cents**. SHIFT + a step adds/subtracts **one cent**.
- Preserve the fractional tuning when making coarse changes: +1.23 becomes +2.23, not +2.00.
- Keep Loop Station transpose unaccelerated so a physical detent remains one semitone or one cent as requested.
- Respect the existing **Horizontal Menus** preference; retain list-based behavior when disabled or on seven-segment hardware.
- Keep live in-memory updates and normal Song Save/load persistence. Do not add per-turn SD writes, new song tags, enum renumbering or migrations.
- Preserve safe SD-card deferral and lazy track binding, including failure feedback.
- A track selected by its ring, or addressed by an open track-settings menu, has a **pulsing ring in its own configured colour**; preserve clockwise playback progress and higher-priority recording/overdub, pending, clear and error indications.
- Do not duplicate horizontal packing, button mapping, paging, rendering or LED algorithms in Loop Station.
- Do not restart the cancelled preview server automatically; run it later only when an actual browser check needs it.
- Keep existing prototype changes and unrelated `.openchamber/` artifacts intact; never add those artifacts to feature commits.

## Review Focus

- Stale sound-editor state on settings entry must never be accessed as Loop Station clip/modulation context or modified by lifecycle/decimal/pad-light calls (Task 1 isolation spies; Task 3 host test).
- A rejected settings command must leave both selected value/notification and model at the authoritative old value (Task 2 rejection test).
- Hiding an interval before the current field must preserve field identity, not edit the field that inherits its old index (Task 2 mode-combination test; Task 3 input test).
- Duplicate button edges, re-selecting Copy's instrument button, and SD-deferred replay must never become unintended copy confirmation (Task 3 confirmation-edge tests).
- Failed opens, closes and cross-view returns must leave no dangling item parents, stale ring-focus override, selected-column LEDs or changed fallback preference (Tasks 1, 3, 4 and 5 lifecycle tests).

---

## Files and responsibilities

- Modify `src/deluge/gui/menu_item/submenu.{h,cpp}`: overridable child relevance with unchanged sound-editor default; route existing relevance checks through it.
- Modify `src/deluge/gui/menu_item/horizontal_menu.{h,cpp}`: narrow lifecycle/permission/action/chain hooks, per-menu acceleration override; keep shared paging/rendering algorithms.
- Modify `src/deluge/gui/menu_item/decimal.cpp`: avoid vertical digit-cursor/global sound-editor state on horizontal editing while retaining shared decimal coarse/fine arithmetic.
- Create `src/deluge/gui/menu_item/loop_station/settings_items.{h,cpp}`: settings context and native Loop Station value/action items.
- Create `src/deluge/gui/menu_item/loop_station/horizontal_settings.{h,cpp}`: external-host `HorizontalMenu` specialization, no duplicate renderer or page arithmetic.
- Modify `src/deluge/gui/menu_item/loop_station/settings.{h,cpp}`: own the context/items/menu, preserve entry/fallback, route input, implement explicit copy review and lifecycle cleanup.
- Modify `src/deluge/loop_station/menu_model.{h,cpp}`: coarse/fine transpose helper and shared scalar field access as needed without changing settings schema.
- Modify `src/deluge/loop_station/geometry.{h,cpp}`: pure, allocation-free visual-clock pulse level alongside existing pulse/progress helpers.
- Modify `src/deluge/gui/views/loop_station_view.{h,cpp}`: focused-ring brightness modulation and redraw scheduling without changing progress, centre/state priority or engine selection.
- Modify `tests/loop_station/{view_tests.cpp,pad_render_tests.cpp}`: pulse phase, focus ownership, colour/progress priority and partial-row renderer coverage.
- Modify `src/deluge/gui/l10n/{strings.h,english.json,seven_segment.json}`: add required short labels/confirmation strings using the existing translation fallback conventions; do not hand-edit generated `g_english.cpp`/`g_seven_segment.cpp` (see that directory's README).
- Modify `src/deluge/CMakeLists.txt` only if new source files are not already discovered by its source glob.
- Extend `tests/loop_station/ui/{generate_probe.py,probe_support.h,probe_support.cpp,fixture.h}`: checked extraction of actual shared methods and boundary stubs; no replacement horizontal logic.
- Create `tests/loop_station/ui/{shared_menu_tests.cpp,settings_items_tests.cpp,horizontal_settings_tests.cpp}`; modify existing `input_tests.cpp`, `menu_tests.cpp`, `persistence_tests.cpp`, and `tests/loop_station/CMakeLists.txt`.
- Extend `tests/oled_canvas/{generate_probe.py,probe_support.h,CMakeLists.txt,loop_station_oled_tests.cpp}`: run real shared menu rendering with real canvas/font geometry.
- Align `website/public/demos/loop-station.html`, `docs/features/loop_station.md`, and `website/src/content/docs/features/loop_station.mdx` after firmware verification.
- Create `docs/superpowers/reports/2026-11-14-loop-station-settings-ui.md` during integration to record evidence and unchecked device acceptance.

## Environment and test conventions

Worktree/isolation choice happens at execution time through `using-git-worktrees`; do not silently move the user's checkout or leave the accepted HTML prototype behind. Preserve the approved spec, this plan and prototype if moving the session.

Run from the repository root with bundled tools on PATH:

```bash
export PATH="$PWD/toolchain/current/python/bin:$PWD/toolchain/current/cmake/bin:$PWD/toolchain/current/clang-format:$PATH"
```

Existing host build: `build/tests`; executable: `build/tests/loop_station/LoopStationTests`. Register the new test files in that target using groups `SharedHorizontalMenu`, `LoopSettingsItems`, and `LoopHorizontalSettings`. The production-probe generator must declare every shared source as a CMake dependency, reject missing/ambiguous extraction markers and preserve extracted bodies unchanged. Stub hardware/editor services, not selection/paging/render/value algorithms. Baseline existing test/build availability before attributing failures to this work.

### Task 1: Make the shared horizontal menu safe for an external host

**Files:** shared `submenu`, `horizontal_menu`, `decimal` files; probe support/generator/CMake; new `ui/shared_menu_tests.cpp`.

**Interfaces produced:** protected virtual `bool Submenu::isChildRelevant(MenuItem* item) const`; protected virtual `void HorizontalMenu::beginHostSession(MenuItem* from)`, `void endHostSession()`, `MenuPermission initializeItem(MenuItem* item)`, `void handleItemAction(MenuItem* item)`, and `bool trySwitchHorizontalMenu(int32_t direction)`; move existing `double calcNextKnobSpeed(int8_t offset)` to protected virtual. `HorizontalMenu::beginSession/endSession` still own parent-pointer and temporary LED cleanup. Existing public signatures and default constructors remain compatible. Existing `updatePadLights()` remains the overridable redraw boundary.

- [ ] **Step 1: Extend the real-method probe and write failing isolation/navigation tests.** Add a test subclass with overridden host hooks and sentinel sound-editor state. `ExternalHostDoesNotTouchEditorState` asserts sentinel range, shortcut coordinates, navigation depth and current clip/modulation pointers are unchanged by begin, select, page, fine edit and end. Spy initialization/action/chain callbacks and assert exactly one dispatch per accepted press. `DefaultHostRetainsEditorServices` asserts default hooks still perform the original begin/end, permission and editor navigation calls. Reuse the real horizontal renderer/paging bodies; no second algorithm in probe support.

```cpp
// Example isolation assertions after an external-host session:
CHECK_EQUAL(7, soundEditor.numberEditPos);
CHECK_EQUAL(1000, soundEditor.numberEditSize);
CHECK_EQUAL(3, soundEditor.numberScrollAmount);
CHECK_EQUAL(9, soundEditor.navigationDepth);
```

- [ ] **Step 2: Establish RED.** Build `LoopStationTests`, then run `build/tests/loop_station/LoopStationTests -sg SharedHorizontalMenu`. After the hook/probe declarations compile, record actual isolation or callback failures; undeclared methods and scaffolding errors alone are not behavioral RED.
- [ ] **Step 3: Implement the hooks without changing existing defaults.** Route all relevance queries in `Submenu` and `HorizontalMenu` through `isChildRelevant`. Move original sound-editor range-preservation code into `beginHostSession`, original base cleanup into `endHostSession`, and current static initialization/action helpers into overridable members. Move sound-editor chain lookup behind `trySwitchHorizontalMenu`; when it returns false, page locally. Keep instrument mapping, debounce, packing, placeholders, accessibility highlighting and notification rendering in shared code. Ensure failed/no-selection paths never dereference `items.end()` or index an empty visible page. Use unscaled `int32_t` offsets when a host opts out of acceleration.
- [ ] **Step 4: Keep horizontal decimal edits independent of the vertical cursor.** In `Decimal::selectEncoderAction(int32_t)`, avoid `scrollToGoodPos()` for horizontal parents; preserve vertical behavior. Use a wide intermediate before clamping if needed for large offsets. Prove with `HorizontalDecimalPreservesFractionAndCursorState`: +123 cents + one normal step is +223; SHIFT step is +124; `soundEditor.numberEditPos/Size/ScrollAmount` remain sentinels. The inherited decimal renderer and value writer remain in use.
- [ ] **Step 5: Establish GREEN and commit.** Run the shared group, existing Loop Station/OLED suites and `git diff --check`; verify default host chain navigation, mixed occupied slots, conditional items, all accessibility modes and LED cleanup with named tests. Commit only Task 1 files: `refactor: support external hosts in horizontal menus`.

### Task 2: Bind native menu items to Loop Station settings

**Files:** new `settings_items.{h,cpp}`; `menu_model.{h,cpp}`; new `ui/settings_items_tests.cpp`; relevant existing model/persistence tests; localization and build registration as required.

**Interfaces consumed:** Task 1 native horizontal decimal behavior and hooks; existing `visibleMenuFields`, `SongModel::dispatch`, `maxDurationBarsForTrack`, palette and interval ordering.

**Interfaces produced:** namespace `deluge::gui::menu_item::loop_station`: `SettingsContext` with `ls::MenuScope scope`, `ls::SlotId slot`, `ls::SongModel* model() const`, `bool relevant(ls::MenuField) const`, `int32_t read(ls::MenuField) const`, `ls::Status write(ls::MenuField,int32_t)`, and `uint16_t maxDurationBars() const`. Scalar values use menu indices for enums/intervals/colours, bars for Duration, cents for Transpose. `SettingsItems(SettingsContext&)` owns stable-lifetime item objects and exposes `std::span<MenuItem*> all()`, `MenuItem* item(ls::MenuField)`, and `ls::MenuField field(const MenuItem*) const` for its own members. `EnumSetting : Selection`, `DurationSetting : Integer`, `TransposeSetting : Decimal`, and `CopySetting : MenuItem` occupy one slot each. Extend the helper to `int16_t stepTransposeCents(int16_t currentCents,int32_t encoderOffset,bool fine=false)`; false means 100-cent steps.

- [ ] **Step 1: Write failing authoritative-value tests.** `NativeFieldsMatchExistingSettings` checks each item's read/write mapping, source choices, both recording actions, both modes, all existing intervals, Manual/dynamic duration, playback and all eleven colours. `VisibilityTracksBothModeFlags` tests all four Start/Stop combinations and exact existing field order. `RejectedWriteRestoresAuthoritativeValue` forces a real invalid/busy configuration and asserts unchanged model value, item cache and notification, without unrelated track changes. Use real `SongModel`, not a second settings implementation.
- [ ] **Step 2: Write failing numeric/persistence tests.** `TransposeCoarseFineAndClamp` pins `stepTransposeCents(123,1,false)==223`, `(123,1,true)==124`, `(-23,-1,false)==-123`, `(2449,1,false)==2450`, `(-2449,-1,true)==-2450`, and safe saturation for `INT32_MIN/MAX`. Native item tests prove SHIFT and held-SELECT fine editing, no editor-cursor mutation and no ordinary SELECT/instrument re-selection action. `EditedTrackSettingsRoundTrip` checks actual item-edited cents, colour and settings against existing encode/decode, retaining slot/audio manifest identities and custom White/duplicates.

```cpp
CHECK_EQUAL(223, stepTransposeCents(123, 1, false));
CHECK_EQUAL(124, stepTransposeCents(123, 1, true));
CHECK_EQUAL(-123, stepTransposeCents(-23, -1, false));
CHECK_EQUAL(2450, stepTransposeCents(2449, 1, false));
CHECK_EQUAL(-2450, stepTransposeCents(-2449, -1, true));
CHECK_EQUAL(2450, stepTransposeCents(0, INT32_MAX, false));
CHECK_EQUAL(-2450, stepTransposeCents(0, INT32_MIN, true));
```

- [ ] **Step 3: Establish RED.** Build and run `-sg LoopSettingsItems`, `-sg LoopMenus`, and `-sg LoopPersistence`. Update the previous one-cent-only helper expectations intentionally; record failing new step-size/binding assertions before implementation.
- [ ] **Step 4: Implement the context and native items.** Extract the field read/configure semantics from the existing settings host into `SettingsContext`, copying authoritative settings immediately before each write. Use positive modulo for enum offsets, dynamic duration limits and the existing command status. Re-read after success or rejection. Override native selection actions to return `NO_NAVIGATION` without sound-editor toggle/navigation calls; keep Copy's action host-owned. Transpose uses the shared `Decimal` for horizontal arithmetic/rendering and its own Loop Station cents binding. Keep fallback helper arithmetic consistent and overflow-safe; do not bind to `AudioClip::sampleHolder`.
- [ ] **Step 5: Establish GREEN and commit.** Run the new group, menu/settings/persistence groups and full `LoopStationTests`; require zero failures, preserving existing capture-configuration behavior. Commit only Task 2 files: `feat: add native loop station settings menu items`.

### Task 3: Integrate the horizontal host and deliberate Copy To flow

**Files:** new `horizontal_settings.{h,cpp}`; settings host; UI probe/fixture and `input_tests.cpp`; new `ui/horizontal_settings_tests.cpp`; build registration.

**Interfaces consumed:** Tasks 1–2 hooks, context and stable item collection.

**Interfaces produced:** `SettingsHorizontalMenu(SettingsContext&,SettingsItems&) : HorizontalMenu`, overriding Task 1 hooks and `updatePadLights()`, and exposing `void resetSelection()`, `MenuField selectedField() const`, `void selectField(MenuField)`. It delegates packing/pages/rendering to its base. `SettingsMenu` owns members in context → items → horizontal-menu lifetime order, adds `bool useHorizontalMenu() const`, `void requestCopyReview()`, `void confirmCopy()`, `void cancelCopyReview()`, and a private copy-review flag/target. Retain `openGlobal`, `openTrack`, and UI method signatures. `calcNextKnobSpeed` returns 1 for the selected Transpose item; other fields retain shared behavior. The horizontal encoder is inert in horizontal mode; retain its current alias only in fallback.

- [ ] **Step 1: Write failing end-to-end selection tests.** `EveryTrackOpensCorrectHorizontalContext` enters via actual SHIFT + each of eight physical centres; assert no loop action and correct addressed slot. `InstrumentButtonsSelectAndSelectRotationEdits` checks all columns/page order, direct edits without an edit toggle, inert empty CV on page 2, page wrapping and shorter-page selection. `ConditionalPageChangesKeepSelectedField` toggles both modes through actual buttons/encoder and asserts the next turn edits the same mode, never its adjacent interval. Reset shared navigation debounce time between fixture events rather than deleting production debounce.
- [ ] **Step 2: Write failing lifecycle/input tests.** `SettingsIgnorePadsAndDoNotAlterTransport` exercises centres/rings/sidebar while horizontal settings is open; phrase state, selection and capture must stay unchanged. `ShiftPagingNeverEntersSoundEditorChain` tests both directions. `MenuExitAndFailedOpenRestoreLedsAndParents` tests BACK, unshifted SONG, shifted SONG staying open, unavailable-track opens, repeated entry/exit and return to an ordinary editor. `TransposeDetentsAreUnaccelerated` rapidly sends actual UI offsets with SHIFT press/release, asserting 100/1 cents per unit; audit `src/deluge/hid/encoder_input.cpp` for upstream multiplication rather than changing it speculatively.
- [ ] **Step 3: Write failing Copy tests.** `CopyNeedsTwoDistinctSelectPresses` selects Copy, rotates among the other seven tracks, presses SELECT to review, then sends releases, repeated on-edges, instrument re-selection and pad presses; destination must remain unchanged. A new SELECT press after release confirms once. `BackCancelsCopyWithoutClosingSettings` and `CopyFailureIsSettingsOnlyAndHonest` cover cancellation, busy/unavailable destination, no SD-locked mutation, preservation of audio/name/output/effects/history and exact success feedback. Test replayed SD-deferred input cannot supply both review and confirm from the same held press.
- [ ] **Step 4: Establish RED.** Build and run `-sg LoopHorizontalSettings` and affected `-sg LoopUi`; record behavioral failures for list-based controls/unsafe copy before wiring the new path.
- [ ] **Step 5: Wire shared menus and lifecycle.** In horizontal mode, route instrument/page/SELECT rotation through the shared menu and select by field identity after relevance changes. Do not call the sound editor to initialize, navigate, clear shortcuts or redraw pads. Route shared redraw requests to the existing Loop Station underlying view. Own Copy's review in `SettingsMenu`, suppressing generic repeated-selection actions; use press/release state to require distinct presses. Run all commands through existing model status checks and existing card-routine deferral. Clean parent pointers/LEDs on all exits and failed opens; redraw parent LEDs after cleanup.
- [ ] **Step 6: Establish GREEN and commit.** Run the new horizontal group, shared group, existing input/pad/capture groups and full `LoopStationTests`; all must pass. Commit only Task 3 files: `feat: integrate shared loop station horizontal settings`.

### Task 4: Pulse the selected or settings-focused track ring — OUT OF SCOPE

> **Execution ruling (November 14, 2026):** User moved the pulse requirement out of this plan for separate planning and implementation. Do not implement pulse/focus override or change redraw scheduling here. Runtime inspection showed the proposed `timerCallback()` is not dispatched while settings is foreground and inherited graphics forwarding triggers a full root render. Task 5 is limited to non-pulse OLED/fallback/documentation work. Keep any HTML prototype pulse clearly labeled illustrative/not implemented in firmware, and preserve the existing user-owned HTML preview. Task 6 may complete the shared settings UI scope while recording the pulse as out-of-scope for this plan; no pulse acceptance claim.

**Files:** geometry visual helpers; Loop Station performance view and settings host; `view_tests.cpp`, `pad_render_tests.cpp`, new horizontal input tests and their probe support. Extend source-extraction dependencies if necessary. Align the docs demo in Task 5, not by silently changing the accepted prototype during this planning step.

**Interfaces consumed (deferred):** Task 3 host lifecycle, current settings scope/slot, `Engine::selected()`, the existing `AudioEngine::loopStationSampleClock()`, `filledBorderPads`, state-priority renderer and configured track colour.

**Interfaces produced:** `uint8_t loop_station::selectionPulseLevel(SampleClock sample)` in `geometry.{h,cpp}` returns a continuous integer triangle wave from 0 to 255 over 66150 samples (1.5 seconds at 44.1 kHz), with level 0 at sample 0/66150 and 255 at 33075. `ls::SlotId SettingsMenu::focusedTrack() const` returns its addressed slot only while that track-settings host is active; otherwise `kInvalidSlot`. `LoopStationView` uses a valid settings focus in preference to `engine.selected()` without modifying selection. Since `SettingsMenu` has no existing graphics-routine callback, add/override `ActionResult SettingsMenu::timerCallback()` and request underlying **pad-only** redraws while settings is foreground; do not invoke the root's `updateLed()` or its full OLED redraw routine from this path. Use a pure renderer-local channel scaler, not new engine or persistent fields.

- [ ] **Step 1: Write failing pulse/focus tests.** `SelectionPulseHasContinuousStablePeriod` asserts the sample values below, both half-cycle directions, period wrap and safe large-clock modulo. `SelectedRingBreathesInOwnColour` presses each of eight physical track rings, renders at low/high phase and asserts only that track's twelve border pads vary while their hue and four centre pads stay unchanged. `TrackMenuFocusOverridesWithoutSelectingEngineTrack` opens slot B settings while A is engine-selected, including fallback and Copy review: only B has the focus pulse; closing restores A, and a failed open leaves A unchanged. Check no added engine command/allocation/transport state change.

```cpp
CHECK_EQUAL(0, selectionPulseLevel(0));
CHECK_EQUAL(255, selectionPulseLevel(33075));
CHECK_EQUAL(0, selectionPulseLevel(66150));
CHECK_EQUAL(selectionPulseLevel(11025), selectionPulseLevel(77175));
```

- [ ] **Step 2: Write failing display-priority/redraw tests.** `FocusPulsePreservesClockwiseProgress` fixes actual audio phase while independently varying `probeSampleClock`, asserting the same filled indices at each pulse phase, with filled brightness always greater than unfilled brightness. `FocusPulseYieldsToStateIndications` covers recording, overdub, pending, error and clear sweep at low/high selection phases: existing state render outputs remain unchanged. `FocusedColourChangesLiveWithoutResettingPulse` covers White/custom duplicate colours and other tracks unchanged. `ForegroundSettingsAnimatesPadsWithoutClobberingMenuLeds` drives graphics ticks with no encoder input and stopped transport, asserting advancing pulse samples/redraw requests without root LED/OLED updates. Retain partial-row sentinel checks and unaffected sidebar rendering.
- [ ] **Step 3: Establish RED.** Build `LoopStationTests` and run `-sg LoopView`, `-sg LoopPadRender` and `-sg LoopHorizontalSettings`; record missing pulse/focus/redraw behavior. Existing exact-colour tests should explicitly render an unselected track or assert the new selected brightness contract, not have their meaningful colour/progress checks deleted.
- [ ] **Step 4: Implement visual-only pulse composition.** Compute phase once per frame from the existing visual sample clock with integer arithmetic. For focus identity rendering use linear 8-bit brightness ranges: playing filled 128→255, playing unfilled 32→64, empty/stopped ring 32→128, interpolated with pulse level 0→255 and rounded channel scaling. This keeps playing progress contrast at least approximately 4:1 while maintaining hue; centre pads and unselected identity rendering are unchanged. Apply only after all current higher-priority state branches. Do not reset pulse phase on selection, menu open or Colour edit. Keep a single focus override, clear it naturally on close/failure, and schedule pad redraws while either settings layout is foreground without stealing menu LEDs or starting a new audio task.
- [ ] **Step 5: Establish GREEN and commit.** Run the three groups and full `LoopStationTests`; require unchanged safety-state outputs, eight-track focus/colour coverage, no allocation and real foreground animation scheduling. Run `git diff --check`; commit only Task 4 files: `feat: pulse the focused loop station track ring`.

### Task 5: Verify real display rendering, fallback and guide alignment (excluding deferred pulse)

**Files:** OLED tests/probes; existing fallback input tests; localization; demo and both feature guides.

**Interfaces consumed:** integrated Task 3 settings UI. No additional runtime API; Task 4 ring focus/animation is deferred.

- [ ] **Step 1: Write failing display/fallback regressions.** Real-canvas tests `SharedSettingsColumnsStayWithinOled` and `CopyReviewFitsOled` render both scopes, both pages, all conditional mode combinations, -24.50/+24.50 and long/localized labels under all supported highlights. Assert canvas bounds/canaries, page text, selection and empty placeholders; use actual shared draw routines and real fonts. `FeatureOffAndSevenSegmentKeepListEditing` checks navigation/edit toggles, existing copy-target flow, semitone/SHIFT-cent steps, readable values and BACK retention in each fallback. `ReturningFromFallbackDoesNotChangePreference` verifies feature state and ordinary editor menu behavior.
- [ ] **Step 2: Establish RED.** Build `LoopStationOledTests`/`OledCanvasTests` and run them plus the fallback group. Record missing shared-render coverage or genuine bounds/value failures, not fabricated rendering expectations.
- [ ] **Step 3: Finish presentation and compatibility.** Use shared title/counter, accessibility, selected LEDs and child renderers; localize compact labels and copy review source/target/confirm/cancel copy. Ensure selected notifications use the authoritative full setting name/value. Retain the existing fallback list renderer/flow; remove row/edit-mode routing only from horizontal mode. No hardcoded eight-bar cap or English-only new firmware labels. Fix legitimate display clipping within the approved shared layout rather than introducing a second renderer.
- [ ] **Step 4: Align documentation and prototype (pulse excluded).** Update both guides with horizontal controls, scope, coarse/fine transpose, copy review, fallback and normal Song Save/load. Do not add or claim a firmware ring pulse. Preserve the accepted HTML interactions and existing user-owned HTML preview unchanged; describe any pulse shown by that prototype as illustrative and not implemented in firmware. Align other behavior that changed during integration. Replace “not implemented firmware” only with accurately qualified implemented/tested status; keep the silent/illustrative audio/memory disclaimer and temporary edits/reset behavior. Preserve the browser's Track settings shortcut and SHIFT toggle as explicitly browser-only conveniences.
- [ ] **Step 5: Establish GREEN and commit.** Run OLED/fallback tests, full host suites and `python3 tests/loop_station/ui/demo_contract_test.py`. Browser-check the existing docs HTML only when needed (a local server may be started then; do not restart it merely because it was cancelled). Check semitone/cents, pages, colour, copy cancel/confirm, global relevance and unchanged loops; do not claim firmware pulse ownership/progress/state-priority acceptance. Record console visibility limits honestly. Run `git diff --check`; commit only Task 5 files: `docs: align loop station controls and display coverage`.

### Task 6: Integrated verification and device handoff

**Files:** create the dated report; update this plan's evidence/checkmarks. No new feature behavior.

**Interfaces consumed:** all earlier tasks' tests and firmware sources; no new runtime APIs.

- [x] **Step 1: Run integrated host verification.** Run `cmake --build build/tests --target LoopStationTests OledCanvasTests LoopStationOledTests LoopAudioAllocationGuard LoopGestureAllocationGuard LoopHistorySnapshotAllocationGuard -j4`, then `ctest --test-dir build/tests --output-on-failure`. Require every suite to pass; record current counts, actual production paths exercised and any unavailable shared/default-host coverage. No “all menus unchanged” claim based only on Loop Station stubs.
- [x] **Step 2: Build Release.** Run `DBT_NO_SYNC=1 ./dbt build Release`; require ARM compilation/link success. Record actual BIN path/size/SHA-256 and embedded source version, with warnings or baseline problems separated from this change.
- [x] **Step 3: Obtain final review under the chosen execution method.** Review shared default behavior, sound-editor isolation, conditional selection, Copy edge ownership, per-item acceleration, actual-render tests and fallback. Ring pulse/focus/redraw behavior is outside this plan and must not be treated as an acceptance criterion here. Final whole-branch review approved with two deferred Minor findings recorded in the SDD ledger/report; no Critical/Important findings.
- [x] **Step 4: Save evidence and an unchecked device checklist.** Report RED/GREEN, host/build/browser/review results and source commits. Device checklist: real OLED labels/values on each page; four column buttons and page LEDs; one-semitone/one-cent physical detents; recording/pending/error/clear indications remain authoritative; entry/exit while loops play/record; no pad-action leakage; deliberate Copy cancel/confirm; feature-off and seven-segment behavior; colours and cents survive normal Song Save/load. Mark unobserved hardware checks **NOT_RUN**. Ring-pulse acceptance belongs to the separate future plan.
- [x] **Step 5: Commit and hand off.** Run `git diff --check`; commit only report/plan evidence with `docs: verify loop station shared settings UI`. Keep the branch, without merging or pushing. Provide the BIN path and outstanding hardware checks. Note that ring-pulse behavior is out of scope for this plan and belongs to separate future planning; do not claim pulse acceptance.

## Plan self-review

- [x] **Spec coverage:** Task 1 owns shared reuse/default compatibility and global-state isolation; Task 2 owns field semantics, localization bindings, coarse/fine arithmetic, rejection and persistence; Task 3 owns real entry/navigation/paging/copy/lifecycle; the focused-ring pulse is moved to a separate future plan by user ruling; Task 5 owns native rendering, preference/seven-segment fallback and non-pulse documentation alignment; Task 6 verifies and hands off this plan's shared settings UI scope with hardware acceptance separate.
- [x] **Step scan:** each task has named failing assertions, a behavioral RED gate, bounded implementation, explicit verification and a scoped commit. Localization source paths were corrected against the project README.
- [x] **Type consistency:** hook signatures, context names and item lifetime order are consistent across tasks; existing public UI/model contracts remain unchanged apart from the explicit defaulted transpose helper argument.
- [x] **Review Focus:** all five conditions have named tests in their owning tasks.
- [x] **Proportion:** the plan records interfaces, assertions and gates rather than full implementation bodies; no new engine/persistence or MIDI/modulation features are included.

The plan was approved for subagent-driven execution on November 14, 2026.
