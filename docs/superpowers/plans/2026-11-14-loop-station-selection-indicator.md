# Loop Station Selected-Track Indicator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the currently selected Loop Station track explicit on its OLED and visibly pulse its ring, with a reviewable HTML preview first.

**Architecture:** Update the existing silent HTML demo before firmware. After a separate user approval of the *actual revised demo*, compose selection brightness only at the end of the firmware view's existing lower-priority border-render branches, and put the selected-track number in the existing root OLED layout. Let the foreground settings host request pad-only redraws without changing engine selection or menu LEDs.

**Tech Stack:** Existing standalone HTML/CSS/JS demo; Playwright already declared in `website/package.json`; C++ Loop Station firmware, host production-source UI probes, OLED canvas tests, CMake/CppUTest.

**Spec:** `docs/superpowers/specs/2026-11-14-loop-station-selection-indicator-design.md` (also respect the compatible focus rules in `docs/superpowers/specs/2026-11-14-loop-station-settings-ui-design.md:118-127`).

## Global Constraints

- Deliver a working revision of `website/public/demos/loop-station.html` first; **STOP and obtain user visual sign-off on that revision before Tasks 2–4**, regardless of plan or design approval. No firmware work may start merely because this plan is approved.
- Only the focused twelve border pads pulse in their configured colour; centre pads, other tracks, clockwise progress geometry/phase and higher-priority error/capture/pending/clear outputs stay intact. Settings focus may differ from engine selection; never dispatch `Select` to show focus.
- Keep the existing OLED memory, unsaved/saved and undo information. No changes to transport, audio, storage schema, recording gestures, allocations or ordinary menus. Reuse existing visual sample clock, UI redraw facilities and palette; no new audio task/dependencies.
- Demo remains silent/illustrative and preserves Pause animation, reduced-motion preference, eight-colour palette and twelve-coordinate ring. Seven-segment fallback must not regress. Device inspection is required for physical visual acceptance.

## File map

- `website/public/demos/loop-station.html`: sole demo UI/state/animation implementation; extend selected label and identity pulse without changing its ring or colour literal arrays.
- `website/scripts/loop-station-selection.test.mjs` (new): Playwright `file://` browser checks for visible label, pulse phases, motion, state precedence and settings. Existing `tests/loop_station/ui/demo_contract_test.py` continues pinning palette/geometry.
- `src/deluge/loop_station/geometry.{h,cpp}`: pure integer triangle level from the existing 44.1 kHz visual sample clock.
- `src/deluge/gui/views/loop_station_view.cpp`: OLED selected-track text, renderer-local pulse composition and pad redraw entry point if required; `loop_station_view.h` only if a public pad-only redraw method proves necessary.
- `src/deluge/gui/menu_item/loop_station/settings.{h,cpp}`: active track focus and foreground tick scheduling, avoiding root OLED/LED calls.
- `tests/loop_station/{geometry_tests.cpp,pad_render_tests.cpp,view_tests.cpp}` and `tests/loop_station/ui/{probe_support.h,fixture.h}`: pure waveform, real production-method pad/gesture/focus/tick regressions; update source extraction only if new methods need it.
- `tests/oled_canvas/{loop_station_oled_tests.cpp,probe_support.h}`: selected-track label and all existing root status rows with real canvas/font and bounds/canaries; update source extraction if needed.

## Review Focus

1. Selecting tracks 1 and 8 via border or encoder must show correct 1-based number without wrap/clipping (Tasks 1, 2, 3).
2. A track-settings menu addressed to B while A remains selected must pulse B only, and close/failed open must restore A (Tasks 1, 4).
3. A stopped/empty track and a playing track must both pulse without hiding progress or restarting its phase (Tasks 1, 3).
4. White or duplicate track colours must not cause selection bleed into other rings; errors, capture, pending and hold clear must remain legible (Tasks 1, 3).
5. Stopped transport, an open fallback/copy-review menu, frozen animation and reduced-motion browser preference must not cause runaway OLED/LED updates or a moving pulse where motion is disabled (Tasks 1, 4).

---

### Task 1: Interactive HTML preview — user review gate

**Files:** Modify `website/public/demos/loop-station.html`; create `website/scripts/loop-station-selection.test.mjs`; verify existing `tests/loop_station/ui/demo_contract_test.py`.

**Interfaces:** Consumes existing `selectTrack(i)`, `updateUI()`, `animate(now)`, `tracks`, `selected`, `settingsScope`, `frozen`, ring-pad `p.ringIndex` and `window.loopStationDemo.snapshot()`. Produces a visible OLED selected-track label and a selected-ring brightness pulse as a reference for firmware; no firmware-facing JS API.

- [ ] **Step 1: Write failing browser tests.** In the new `.mjs` Playwright script, navigate to `file://` for the resolved demo path. Tap a border pad for tracks 1 and 8; assert a clear `TRACK 1`/`TRACK 8` selected label on the mock OLED and that all twelve border pad computed colours vary over two sample times while the four centres and one other track do not. Check empty/stopped and playing tracks, unchanged clockwise lit indices, selecting a new track without resetting progress, White/duplicate colours, state precedence (recording/overdub, hold clear), settings focus, Pause animation and `page.emulateMedia({ reducedMotion: 'reduce' })` (static selected highlight, no brightness motion). Use `window.loopStationDemo.snapshot()` plus rendered pad style/DOM, not just source-text matching; retain the existing settings controls and accessibility labels.
- [ ] **Step 2: Verify RED.** Run `node website/scripts/loop-station-selection.test.mjs` from repo root (website dependencies and browser installed). Expect assertion failure for missing OLED selection emphasis and selected brightness variation, not a missing dependency/browser failure.
- [ ] **Step 3: Update the existing demo.** In `updateUI()` make the selected number unmistakable within the OLED, including after selection via settings/other existing shortcuts. In `animate(now)`, reuse the existing record/overdub pulse value for selected-ring brightness only after the existing higher-priority ring states, preserving the ring index/head logic and colour; the ring's configured colour may change live. A settings track, when active, is the focus; global settings retain engine selection. Pausing freezes phase; reduced motion presents a steady high-contrast selected ring without continuous movement. Keep the eight `colors` and twelve `ring` literal arrays unchanged; keep illustrative status wording.
- [ ] **Step 4: Verify GREEN and inspect.** Run `node website/scripts/loop-station-selection.test.mjs` and `python3 tests/loop_station/ui/demo_contract_test.py`; expect both exit 0. Open the revised demo in a browser and check selection, progress and settings at desktop/mobile widths. Run `git diff --check`.
- [ ] **Step 5: Commit the preview.** `git add website/public/demos/loop-station.html website/scripts/loop-station-selection.test.mjs && git commit -m "feat: preview selected loop track pulse"`.
- [ ] **Step 6: STOP — present the actual revised `loop-station.html` to the user and obtain explicit visual approval (or revise and repeat).** Neither design/plan approval nor browser-test success substitutes for this gate. Do not begin Tasks 2–4 without this new approval.

### Task 2: Selected-track OLED indicator

**Files:** Modify `src/deluge/gui/views/loop_station_view.cpp:382-395`, `tests/oled_canvas/loop_station_oled_tests.cpp:115-159` and, if probe extraction requires it, `tests/oled_canvas/probe_support.h`/its generator.

**Interfaces:** Consumes `m->engine().selected()`, `validSlot(SlotId)` and existing canvas draw methods; produces a legible root OLED label `Track N` (N=1..8), while retaining existing memory/save/undo status rows. Settings screens retain their own titles.

- [ ] **Step 1: Write failing actual-canvas tests.** Extend `testRootShowsAllStatusRows` for selected slots 0 and 7 and both memory profiles × saved/unsaved × undo states: selected track is legible; all old rows remain unchanged. Include invalid selection and null model behavior without out-of-bounds reads or an invented selected track, and canary checks at 128-pixel width. Compare actual pixels using real fonts; do not trust only a text-recording mock.
- [ ] **Step 2: Verify RED.** Build `LoopStationOledTests` via `cmake --build build/tests --target LoopStationOledTests -j4` and run `ctest --test-dir build/tests -R '^LoopStationOledTests$' --output-on-failure`; expect image/assertion failure for missing track label.
- [ ] **Step 3: Implement the indicator.** Modify `LoopStationView::renderOLED(Canvas&)` to draw explicit selected-track text using a bounded small stack buffer or stable literal/number construction, not dynamic allocation. Fit it alongside/within the title using real font dimensions; preserve all three status rows and the model-absent title-only path. Follow the visually approved demo without assuming mock OLED pixel geometry equals real OLED geometry.
- [ ] **Step 4: Verify GREEN and commit.** Run the same target/CTest and `git diff --check`; expect pass and intact canaries. Commit only this task's files: `feat: show selected loop track on OLED`.

### Task 3: Renderer-local ring pulse

**Files:** Modify `src/deluge/loop_station/geometry.{h,cpp}`, `src/deluge/gui/views/loop_station_view.cpp:287-349`, `tests/loop_station/{geometry_tests.cpp,pad_render_tests.cpp,view_tests.cpp}` and production-method probe dependencies as needed.

**Interfaces:** Consumes existing `AudioEngine::loopStationSampleClock()`, `visualPulseOn(sample, 11025)`/the record-overdub visual period (22050 samples at 44.1 kHz), `Engine::selected()`, configured colour, `filledBorderPads` and `SettingsMenu::focusedTrack()` from Task 4 (for a phased build, first render engine selection; integrate the menu focus in Task 4). If smooth selection brightness needs a pure integer phase helper, add `uint8_t loop_station::selectionPulseLevel(SampleClock sample)` that shares the record/overdub period. No engine or persistence field is added.

- [ ] **Step 1: Write failing waveform/renderer tests.** If a smooth phase helper is needed, pin period wrap at 22050 samples, low/high phases 11025 samples apart, and safe huge-clock modulo; ensure selection's full brightness cycle matches record/overdub's existing visual rate. For each of eight physical rings select via real border press, render at the corresponding low/high phases with fixed audio phase and assert only its twelve border RGBs vary in the configured hue, centres and other rings remain identical. Test stopped/empty and playing; fixed clockwise filled count and filled brightness greater than unfilled at both phases; White/duplicate colours and live colour edits. Verify priority outputs unchanged at both phases for error, capture/overdub, pending and clear sweep; partial rows and sidebar untouched. Update old exact-colour assertions to target an unselected ring or assert pulse contract rather than deleting their coverage.
- [ ] **Step 2: Verify RED.** `cmake --build build/tests --target LoopStationTests -j4 && build/tests/loop_station/LoopStationTests -sg LoopPadRender` plus geometry/view groups; once declarations/probes compile, expect missing pulse behaviour (not an extraction/compilation error).
- [ ] **Step 3: Implement integer waveform and visual composition.** If required, add `selectionPulseLevel(SampleClock)` in `geometry.{h,cpp}` with widened modulo arithmetic and a 22050-sample period matching the existing record/overdub visual pulse, not a new independent 1.5-second clock. In `LoopStationView::renderMainPads`, compute visual clock/level once per frame; apply integer per-channel brightness scaling to the focused border only in ordinary playing/stopped/empty branches, after all state overrides. Suggested levels: playing filled 128→255, unfilled 32→64, stopped/empty 32→128; keep hue, filled>unfilled, and original progress index. Do not use floating point, audio commands, heap or SD activity on this path.
- [ ] **Step 4: Verify GREEN and commit.** Run full `LoopStationTests`, `ctest --test-dir build/tests -R '^(LoopStationTests|LoopAudioAllocationGuard|LoopGestureAllocationGuard)$' --output-on-failure`, `git diff --check`; expect pass. Commit only this task's files: `feat: pulse selected loop ring`.

### Task 4: Foreground settings focus and animation scheduling

**Files:** Modify `src/deluge/gui/menu_item/loop_station/settings.{h,cpp}`, `src/deluge/gui/views/loop_station_view.{h,cpp}` if a narrow pad-only redraw hook is needed, `tests/loop_station/{pad_render_tests.cpp,view_tests.cpp}` and `tests/loop_station/ui/{probe_support.h,fixture.h}`/extraction dependencies as needed.

**Interfaces:** Produce `ls::SlotId SettingsMenu::focusedTrack() const`: addressed slot only while an actual track-settings UI is foreground and successfully open; otherwise `ls::kInvalidSlot`. `LoopStationView` uses valid focus before `engine.selected()` in pad rendering; do not mutate engine selection. `SettingsMenu::timerCallback() -> ActionResult` (if UI dispatch confirms this callback is actually delivered) requests only underlying pad redraw while foreground, including fallback and Copy review. Root `graphicsRoutine()` is forbidden here because it updates parent LEDs and OLED.

- [ ] **Step 1: Write failing UI/probe tests.** Select A, open B's track settings and verify B alone pulses at two clock phases while engine selection remains A; repeat with global settings, fallback list, Copy review, failed track open, close and exit, checking focus restoration. Drive actual UI timer/graphics dispatch with stopped transport and no input; verify pad refreshes across phases without invoking root OLED redraw or `updateLed()`, without clearing selected menu LEDs, and without a new allocation/transport command. Verify the timer route rather than assuming `timerCallback()` is called: the November 14 plan records it is **not** dispatched while settings is foreground, so plan for the actual UI dispatch seam if necessary.
- [ ] **Step 2: Verify RED.** `cmake --build build/tests --target LoopStationTests -j4 && build/tests/loop_station/LoopStationTests -sg LoopUi && build/tests/loop_station/LoopStationTests -sg LoopPadRender`; expect behavioural focus/redraw failures after probe declarations compile.
- [ ] **Step 3: Implement the narrow foreground hook.** Add `focusedTrack() const` and integrate it into the renderer; use the existing firmware graphics/tick path that actually runs with settings foreground, requesting pad-only redraw. Do not schedule a second audio task, re-render parent OLED, or steal menu LEDs. Guard failed opens, closing, song/model absence and partial-row requests.
- [ ] **Step 4: Verify GREEN and integrated firmware.** Build `LoopStationTests OledCanvasTests LoopStationOledTests LoopAudioAllocationGuard LoopGestureAllocationGuard LoopHistorySnapshotAllocationGuard` in `build/tests`; run `ctest --test-dir build/tests --output-on-failure` and `DBT_NO_SYNC=1 ./dbt build Release`; expect all pass, then `git diff --check`. Record physical-device OLED contrast/pulse/LED observations as a separate required manual check, not inferred from host tests. Commit task files: `feat: animate focused loop ring in settings`.

## Plan handoff

After approval of this plan, choose the execution mode separately. The implementer must stop at Task 1's visual sign-off gate and wait for a new user decision before any production firmware changes. This planning session makes no production-code changes.
