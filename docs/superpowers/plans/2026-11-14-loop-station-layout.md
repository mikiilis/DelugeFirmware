# Loop Station Physical Layout and Ring Colours Implementation Plan

**Status:** Implementation authorized by the user's November 14, 2026 instruction, "Implement this plan". Tasks 1–2 implemented and task-reviewed; Task 3 host/build/visual-browser evidence and approved final integrated review recorded in `docs/superpowers/reports/2026-11-14-loop-station-layout.md`. Dedicated console verification and hardware acceptance remain NOT_RUN.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Match the HTML's top-left, clockwise eight-track layout on the device, provide eight distinct non-white default rings, and allow each track's colour to be changed in its configuration.

**Architecture:** Normalize physical Y coordinates only inside Loop Station's shared geometry, with an explicit physical top-row constant for sidebar drawing/input. Keep logical track identities unchanged. Extend the existing production-handler probe to compile actual pad renderers against the real model, then change the default palette and its demo counterpart.

**Tech Stack:** C++23, CppUTest, Python 3 source extraction/unittest, CMake/CTest, existing DBT ARM toolchain; static HTML demo.

**Spec:** `docs/superpowers/specs/2026-11-14-loop-station-layout-design.md`

## Global Constraints

- No recording/timing/gesture/engine changes, no schema or enum renumbering, no global coordinate inversion, no forced recolouring of saved songs, and no unrelated input-edge fixes.
- Keep previous lazy binding, Shift, SD deferral and OLED updates intact.
- No new dependencies.
- Preserve explicitly saved/custom colours—including White—and permit duplicate user-selected/copied colours; do not silently recolour existing songs or introduce a persistence migration.
- Device inspection is still required to establish perceived colour separation and actual LED orientation; host tests do not establish hardware contrast.

## Review Focus

- Physical inputs and rendered tiles must name the same slot; avoid a second Y inversion at callers.
- All 12 border steps and clear sweeps must agree in both tile rows, including phase-zero and wrap boundaries.
- A partial row redraw must not mutate other rows; invalid coordinates must be rejected before conversion.
- Sidebar drawing and actions must move together; bottom/unused sidebar presses must do nothing.
- Each track's Colour configuration must update only its ring, survive BACK and Song Save/load, and preserve custom White/duplicates while new defaults remain distinct.

---

## Files and responsibilities

- `src/deluge/loop_station/geometry.{h,cpp}`: physical-to-visual mapping and coordinate documentation.
- `src/deluge/gui/views/loop_station_view.cpp`: physical sidebar row and Loop Station Pink mapping; no global LED changes.
- `src/deluge/loop_station/types.h`: default colour per logical slot, preserving enum values.
- `src/deluge/gui/menu_item/loop_station/settings.cpp`: reuse the existing per-track Colour row and schedule visible ring feedback after edits.
- `src/deluge/loop_station/menu_model.h`: existing `kTrackColours`/`trackColourAt` contract; retain its 11 choices and IDs, no new picker.
- `tests/loop_station/view_tests.cpp`: independent physical-coordinate and phase tables.
- `tests/loop_station/ui/{generate_probe.py,probe_support.h,probe_support.cpp,fixture.h,input_tests.cpp}`: actual renderer extraction, minimal hardware boundaries, and corrected physical interaction fixtures.
- Create `tests/loop_station/pad_render_tests.cpp`: real-model/production-renderer integration assertions (accepted file-placement ruling: existing root-level domain test registration).
- `tests/loop_station/{CMakeLists.txt,settings_tests.cpp,persistence_tests.cpp}`: test registration, defaults and saved-data invariants.
- `website/public/demos/loop-station.html`: replace only the sixth default swatch; keep reference geometry unchanged.
- Create `tests/loop_station/ui/demo_contract_test.py`: dependency-free checks of the demo's literal colour/ring arrays.
- `website/src/content/docs/features/loop_station.mdx`: document physical orientation and new-default/custom-colour policy.
- Create `docs/superpowers/reports/2026-11-14-loop-station-layout.md`: implementation evidence and device checklist during execution, not this planning task.

## Execution environment

Run from the repository root. Prepend bundled tools when invoking CMake/CTest/DBT/git hooks:

```bash
export PATH="$PWD/toolchain/current/python/bin:$PWD/toolchain/current/cmake/bin:$PWD/toolchain/current/clang-format:$PATH"
```

Existing host build is `build/tests`; test binary is `build/tests/loop_station/LoopStationTests`. The user's November 14, 2026 "Implement this plan" authorized execution in the current feature checkout. Existing subagent-driven execution preference is retained; the controller owns final independent review. Keep the branch without merge/push.

### Task 1: Correct physical orientation end-to-end

**Files:** geometry, performance view, existing geometry/input tests and UI probe files above; create `pad_render_tests.cpp`; register it in `tests/loop_station/CMakeLists.txt`.

**Interfaces:** Retain `PadAddress padAddress(int32_t x,int32_t y)`, `uint8_t filledBorderPads(FrameIndex,FrameIndex)` and `uint8_t clearHoldSweepPads(SampleClock,SampleClock)`. Extend probe declarations with the real `renderMainPads(uint32_t,RGB[][kDisplayWidth+kSideBarWidth],uint8_t[][kDisplayWidth+kSideBarWidth],bool)` and `renderSidebar(uint32_t,RGB[][kDisplayWidth+kSideBarWidth],uint8_t[][kDisplayWidth+kSideBarWidth])` signatures. Stub only `uint64_t AudioEngine::loopStationSampleClock()` with controllable test time; use real RGB/palette/view-state helpers and real SongModel/Engine.

- [x] **Step 1: Write failing geometry and interaction regressions.** Replace misleading `topLeft=(0,0)` assertions with physical tables. Test `PhysicalRowsMapToHtmlTrackOrder`, `EveryTileRingStartsTopLeftAndRunsClockwise`, and `InvalidPhysicalCoordinatesStayInvalid`. For each tile use an independent literal origin table `{{0,7},{4,7},{8,7},{12,7},{0,3},{4,3},{8,3},{12,3}}`; expected centre offsets are `{1,-1},{2,-1},{1,-2},{2,-2}`. Use the ring table below, not expectations calculated by `padAddress` itself:

```cpp
constexpr int32_t clockwiseOffsets[12][2] = {
    {0,0}, {1,0}, {2,0}, {3,0}, {3,-1}, {3,-2},
    {3,-3}, {2,-3}, {1,-3}, {0,-3}, {0,-2}, {0,-1}
};
CHECK_EQUAL(0, padAddress(0,7).slot);
CHECK_EQUAL(4, padAddress(0,3).slot);
CHECK_EQUAL(7, padAddress(12,0).slot);
CHECK_FALSE(padAddress(-1,7).valid);
CHECK_FALSE(padAddress(0,8).valid);
```

Update existing performance-view test coordinates with literal physical positions for their same intended logical slot (for example slot 0 centre `(1,6)`, slot 6 centre `(9,2)`, slot 7 border `(12,3)`). Update `Fixture::recordPhrase()` to use `(1,6)`. Move existing active sidebar test coordinates to `y=7`; add `OnlyPhysicalTopSidebarControlsDispatch`, asserting bottom/unused rows do not stop a playing phrase or allocate clips. Preserve original behavioral assertions, including SD deferral and Shift.

- [x] **Step 2: Add failing production-renderer tests.** Extend checked extraction to include the existing anonymous helper block plus performance methods through `renderSidebar`, keeping their bodies unmodified. Include real `gui/colour/palette.h` and `loop_station/view_state.h`, add probe renderer declarations, and register the new test file. Tests `TopLeftSlotAndInputAgree`, `PlayingRingVisitsPhysicalClockwisePads`, `ClearSweepUsesSamePhysicalOrder`, `SidebarColoursAndActionsShareTopRow`, and `PartialRowsLeaveOtherPixelsUntouched` must compare actual RGB/mask buffers at physical `[y][x]` coordinates. At phase 0 of a 120-frame slot-0 phrase, `(0,7)` is bright Yellow; at frames 10,20,...,110 successive ring pads become bright; frame 120 resets to one. Check slot 4 independently at physical bottom-left, and leave sentinel pixels outside the requested row unchanged. Use real phrase recording/rendering to set phase/state; do not mock the geometry or engine.
- [x] **Step 3: Establish RED.** Run `cmake --build build/tests --target LoopStationTests -j4`, then `build/tests/loop_station/LoopStationTests -sg LoopView`, `-sg LoopUi`, and the new `-sg LoopPadRender`. Record behavioral failures for mirrored slot/ring/sidebar expectations; scaffolding compile errors do not count as RED.
- [x] **Step 4: Repair mapping.** In `padAddress`, retain bounds checks, then compute `visualY=7-y` and use it for slot and local Y calculations. Document physical `y=0` bottom/`y=7` top in `geometry.h`. Introduce one view-local constant `kSidebarControlY=kDisplayHeight-1` and use it in both `padAction` and `renderSidebar`. Do not transform in renderer/input callers, invert image buffers, reorder model slots or change ring timing/count functions.
- [x] **Step 5: Establish GREEN.** Rebuild and run all three groups, then full `LoopStationTests`; require zero failures. Verify updated regression coordinates still exercise lazy binding, Shift and SD deferral rather than deleting those tests.
- [x] **Step 6: Review and commit.** Run `git diff --check`, review input/render symmetry, then commit only this task's implementation/tests with message `fix: align loop station pads with physical top-left layout`.

### Task 2: Distinct defaults and configurable per-track colours

**Files:** `types.h`, performance view, `src/deluge/gui/menu_item/loop_station/settings.cpp`, default/persistence/input/pad-render tests, UI probe boundary declarations as needed, `loop-station.html`, feature guide; create `demo_contract_test.py` and register it in CMake. Reuse `menu_model.h` without changing palette IDs/order.

**Interfaces:** Retain `TrackSettings defaultTrackSettings(SlotId)`, `Settings defaultSettings()`, all `Colour` enum IDs, and the existing private renderer `RGB colourFor(Colour)`. Reuse `SettingsMenu::openTrack(SlotId)`, `selectEncoderAction(int8_t)`, `buttonAction(Button,bool,bool)`, `changeValue(int32_t)`, `MenuField::Colour`, `kTrackColours`, and `trackColourAt(unsigned)`. Consume Task 1's renderer probe. Existing `Persistence::encode(const Settings&,const Manifest&,SettingsDocument&)` and `decode(const SettingsDocument&,Settings&,Manifest&)` remain unchanged.

- [x] **Step 1: Write failing palette tests.** Update `LoopSettings::trackDefaultsAndSlots` to expect `{Yellow,Cyan,Blue,Purple,Magenta,Pink,Gold,Lime}`. Add `DefaultRenderedRingsAreDistinctAndChromatic`: through the actual renderer, verify all eight empty-track borders use unique RGBs and each has `max(r,g,b)-min(r,g,b)>0`; empty centres remain `(40,40,40)`. For slot 5, stopped/empty ring is `RGB(255,0,128).dim(3)` and a filled playing ring is `RGB(255,0,128)`. Verify centre recording/playing colours remain Red/Green and existing temporary ring state overrides remain unchanged.
- [x] **Step 2: Test configuration access and live feedback.** Add `EveryTrackConfigOffersColourAndChangesOnlyItsRing` covering all eight physical centre locations through SHIFT + centre, then navigate the real menu to `MenuField::Colour`, press SELECT, turn SELECT, and finish editing. Assert the chosen track's model colour changes, its rendered identity border changes, a root pad redraw and menu OLED refresh are requested, and other tracks' settings/clip identities remain unchanged. Add `ColourPaletteWrapsAndBackKeepsChoice`: Gold stepped backward selects White; White stepped forward selects Gold; BACK returns to the performance view retaining the chosen colour. Keep temporary recording/error colours authoritative. Extend the UI boundary probe with `getRootUI()` and redraw-target capture if needed, not a duplicate menu implementation. Existing colour-edit behavior may already pass; identify any missing feedback with a genuine failing assertion rather than manufacturing RED.
- [x] **Step 3: Pin persistence boundaries.** Add `CustomWhiteAndDuplicateColoursKeepTheirSlots` to `persistence_tests.cpp`: encode/decode a document with White on slots 0 and 5, distinct transpose/settings values per slot, and existing manifest phrase identities; assert the same slots, colours, settings and phrase identities survive. Add `ConfiguredTrackColoursRoundTripAfterEditing` to `ui/input_tests.cpp`, including `deluge/loop_station/persistence.h` there: use settings produced by actual per-track menu edits for all eight slots and the real persistence encoder/decoder; verify the edited colours restore to the same logical tracks. These preservation tests may already pass; do not manufacture RED or add migration logic.
- [x] **Step 4: Add a demo contract check.** Implement dependency-free Python unittest reading the named literal `colors` and `ring` arrays from `website/public/demos/loop-station.html`. Test `test_eight_distinct_chromatic_defaults` (eight distinct RGBs, each channel spread at least 64, sixth exactly `#ff0080`) and `test_top_left_clockwise_ring_unchanged` (exact original 12-coordinate reference ring). Fail on missing/ambiguous array markers, not a silent skip. Register `LoopStationDemoContract` with the existing Python interpreter in CMake; do not install packages.
- [x] **Step 5: Establish RED.** Rebuild and run `LoopSettings`, `LoopPadRender`, `LoopUi`, and `python3 tests/loop_station/ui/demo_contract_test.py`; record expected default/Pink/demo failures and any missing configuration feedback. Run `LoopPersistence` to confirm the preservation baseline.
- [x] **Step 6: Update defaults/local display mapping and reuse track configuration.** Replace slot 5 White with Pink in `defaultTrackSettings`; change only Loop Station's `Colour::Pink` mapping to `RGB(255,0,128)`. Keep White selectable and renderable, keep enum order/IDs and all other colour mappings. Reuse the already present Colour row and `SettingsMenu::changeValue(int32_t)`; after a successful Colour configuration dispatch, schedule underlying ring redraw through `uiNeedsRendering(getRootUI())`, while retaining `refreshDisplay()` for menu feedback. Do not add a duplicate picker or write to SD on each turn. Change only `colors[5]` in the demo to `#ff0080`; leave its ring, CSS row order, timing and interaction code unchanged. Document **SHIFT + track centre → Colour → SELECT to edit → turn SELECT → SELECT to finish → BACK**, normal Song Save/load persistence, new defaults and corrected physical top-row sidebar in the feature guide.
- [x] **Step 7: Establish GREEN and commit.** Rebuild, run `LoopSettings`, `LoopPadRender`, `LoopPersistence`, `LoopUi`, the Python demo contract and full `LoopStationTests`; require zero failures. Run `git diff --check` and commit only task files with message `fix: distinguish and configure loop station ring colours`.

### Task 3: Integration verification and device handoff

**Files:** create the dated verification report listed above; update plan checkboxes only when supported by evidence. No further feature changes planned.

**Interfaces:** Consume Tasks 1–2's tests, physical coordinate contract and produced firmware sources; no new runtime APIs.

- [x] **Step 1: Run host verification.** `cmake --build build/tests --target LoopStationTests OledCanvasTests LoopStationOledTests LoopAudioAllocationGuard LoopGestureAllocationGuard LoopHistorySnapshotAllocationGuard -j4`, then `ctest --test-dir build/tests --output-on-failure`. Require all suites including `LoopStationDemoContract` to pass; record exact current counts instead of copying prior counts.
- [ ] **Step 2: Inspect the demo without redesigning it.** Open `website/public/demos/loop-station.html` through a local browser using available browser tools. Confirm unchanged track order and clockwise motion, new sixth swatch, and no browser-console errors. If browser tooling is unavailable, record that check as NOT_RUN; the Python contract is not a browser acceptance claim.
- [x] **Step 3: Build Release.** Run `DBT_NO_SYNC=1 ./dbt build Release`; require successful ARM compilation/link. Inspect the actual BIN's path, size, SHA-256 and embedded source version; cached filename hashes may be stale. Record warnings transparently and do not change unrelated configuration/toolchain overrides.
- [x] **Step 4: Review the integrated change.** Obtain independent review according to the retained execution method, focusing on no double inversion, slot/persistence compatibility, partial-row rendering and shared sidebar position. Fix actionable findings, rerun affected tests, and run `git diff --check` before committing evidence.
- [x] **Step 5: Document and hand off.** Save RED/GREEN, host/build/browser/review evidence and an unchecked hardware list in `docs/superpowers/reports/2026-11-14-loop-station-layout.md`; commit with message `docs: verify loop station layout and palette repair`. Provide the actual BIN path. Hardware checklist: new song top-left Yellow and eight distinct rings; all rings/clear sweeps top-left clockwise; physical press/selection matches track number/settings/clip; each track's Colour config changes only its identity ring, BACK retains it and Song Save/load restores it; top sidebar Start/Stop only; existing song slot/audio/custom White unchanged. Explicitly mark device work NOT_RUN until observed.

Task 3 evidence: host 336/336 Loop Station tests and 20/20 CTest suites PASS; Release ARM compile/link PASS. Step 2 visual browser inspection PASS, but dedicated console verification is NOT_RUN (tool schema exposes no console log; local server also observed an automatic `/favicon.ico` 404). Step 4's independent integrated review approved `22b11a86..3e1a06e1` with no change-specific findings. Controller reran all 20 CTest suites successfully. Neither host nor browser evidence is hardware acceptance.

## Plan self-review

Spec coverage: Task 1 covers complete physical geometry, input/render/sidebar agreement and unchanged slot identities; Task 2 covers defaults, per-track Colour configuration/live feedback, palette wrapping, demo parity and saved/custom-colour restoration; Task 3 covers integration/build/review and explicitly separate hardware acceptance. All five review-focus conditions have named tests. Public signatures and persistence contracts are unchanged; the new probe uses production renderer bodies, not an independent renderer implementation. The user's November 14 "Implement this plan" authorized the proposed colour choices and saved-song policy; independent integrated review and hardware acceptance are separate outstanding gates.
