# Loop Station Physical Layout and Ring Colours Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Match the HTML's top-left, clockwise eight-track layout on the device and remove White from the eight default ring colours.

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
- Saved slot/audio identities and custom White/duplicate colours must survive unchanged while new defaults are non-white and distinct.

---

## Files and responsibilities

- `src/deluge/loop_station/geometry.{h,cpp}`: physical-to-visual mapping and coordinate documentation.
- `src/deluge/gui/views/loop_station_view.cpp`: physical sidebar row and Loop Station Pink mapping; no global LED changes.
- `src/deluge/loop_station/types.h`: default colour per logical slot, preserving enum values.
- `tests/loop_station/view_tests.cpp`: independent physical-coordinate and phase tables.
- `tests/loop_station/ui/{generate_probe.py,probe_support.h,probe_support.cpp,fixture.h,input_tests.cpp}`: actual renderer extraction, minimal hardware boundaries, and corrected physical interaction fixtures.
- Create `tests/loop_station/ui/pad_render_tests.cpp`: real-model/production-renderer integration assertions.
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

Existing host build is `build/tests`; test binary is `build/tests/loop_station/LoopStationTests`. Use the current approved checkout preference unless the user changes it. Existing subagent-driven execution preference is retained, but this request creates a plan only: obtain review/authorization before execution.

### Task 1: Correct physical orientation end-to-end

**Files:** geometry, performance view, existing geometry/input tests and UI probe files above; create `pad_render_tests.cpp`; register it in `tests/loop_station/CMakeLists.txt`.

**Interfaces:** Retain `PadAddress padAddress(int32_t x,int32_t y)`, `uint8_t filledBorderPads(FrameIndex,FrameIndex)` and `uint8_t clearHoldSweepPads(SampleClock,SampleClock)`. Extend probe declarations with the real `renderMainPads(uint32_t,RGB[][kDisplayWidth+kSideBarWidth],uint8_t[][kDisplayWidth+kSideBarWidth],bool)` and `renderSidebar(uint32_t,RGB[][kDisplayWidth+kSideBarWidth],uint8_t[][kDisplayWidth+kSideBarWidth])` signatures. Stub only `uint64_t AudioEngine::loopStationSampleClock()` with controllable test time; use real RGB/palette/view-state helpers and real SongModel/Engine.

- [ ] **Step 1: Write failing geometry and interaction regressions.** Replace misleading `topLeft=(0,0)` assertions with physical tables. Test `PhysicalRowsMapToHtmlTrackOrder`, `EveryTileRingStartsTopLeftAndRunsClockwise`, and `InvalidPhysicalCoordinatesStayInvalid`. For each tile use an independent literal origin table `{{0,7},{4,7},{8,7},{12,7},{0,3},{4,3},{8,3},{12,3}}`; expected centre offsets are `{1,-1},{2,-1},{1,-2},{2,-2}`. Use the ring table below, not expectations calculated by `padAddress` itself:

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

- [ ] **Step 2: Add failing production-renderer tests.** Extend checked extraction to include the existing anonymous helper block plus performance methods through `renderSidebar`, keeping their bodies unmodified. Include real `gui/colour/palette.h` and `loop_station/view_state.h`, add probe renderer declarations, and register the new test file. Tests `TopLeftSlotAndInputAgree`, `PlayingRingVisitsPhysicalClockwisePads`, `ClearSweepUsesSamePhysicalOrder`, `SidebarColoursAndActionsShareTopRow`, and `PartialRowsLeaveOtherPixelsUntouched` must compare actual RGB/mask buffers at physical `[y][x]` coordinates. At phase 0 of a 120-frame slot-0 phrase, `(0,7)` is bright Yellow; at frames 10,20,...,110 successive ring pads become bright; frame 120 resets to one. Check slot 4 independently at physical bottom-left, and leave sentinel pixels outside the requested row unchanged. Use real phrase recording/rendering to set phase/state; do not mock the geometry or engine.
- [ ] **Step 3: Establish RED.** Run `cmake --build build/tests --target LoopStationTests -j4`, then `build/tests/loop_station/LoopStationTests -sg LoopView`, `-sg LoopUi`, and the new `-sg LoopPadRender`. Record behavioral failures for mirrored slot/ring/sidebar expectations; scaffolding compile errors do not count as RED.
- [ ] **Step 4: Repair mapping.** In `padAddress`, retain bounds checks, then compute `visualY=7-y` and use it for slot and local Y calculations. Document physical `y=0` bottom/`y=7` top in `geometry.h`. Introduce one view-local constant `kSidebarControlY=kDisplayHeight-1` and use it in both `padAction` and `renderSidebar`. Do not transform in renderer/input callers, invert image buffers, reorder model slots or change ring timing/count functions.
- [ ] **Step 5: Establish GREEN.** Rebuild and run all three groups, then full `LoopStationTests`; require zero failures. Verify updated regression coordinates still exercise lazy binding, Shift and SD deferral rather than deleting those tests.
- [ ] **Step 6: Review and commit.** Run `git diff --check`, review input/render symmetry, then commit only this task's implementation/tests with message `fix: align loop station pads with physical top-left layout`.

### Task 2: Eight distinct non-white defaults, without saved-data migration

**Files:** `types.h`, performance view, default/persistence/pad-render tests, `loop-station.html`, feature guide; create `demo_contract_test.py` and register it in CMake.

**Interfaces:** Retain `TrackSettings defaultTrackSettings(SlotId)`, `Settings defaultSettings()`, all `Colour` enum IDs, and the existing private renderer `RGB colourFor(Colour)`. Consume Task 1's renderer probe. Existing `Persistence::encode(const Settings&,const Manifest&,SettingsDocument&)` and `decode(const SettingsDocument&,Settings&,Manifest&)` remain unchanged.

- [ ] **Step 1: Write failing palette tests.** Update `LoopSettings::trackDefaultsAndSlots` to expect `{Yellow,Cyan,Blue,Purple,Magenta,Pink,Gold,Lime}`. Add `DefaultRenderedRingsAreDistinctAndChromatic`: through the actual renderer, verify all eight empty-track borders use unique RGBs and each has `max(r,g,b)-min(r,g,b)>0`; empty centres remain `(40,40,40)`. For slot 5, stopped/empty ring is `RGB(255,0,128).dim(3)` and a filled playing ring is `RGB(255,0,128)`. Verify centre recording/playing colours remain Red/Green and existing temporary ring state overrides remain unchanged.
- [ ] **Step 2: Pin persistence boundaries.** Add `CustomWhiteAndDuplicateColoursKeepTheirSlots` to `persistence_tests.cpp`: encode/decode a document with White on slots 0 and 5, distinct transpose/settings values per slot, and existing manifest phrase identities; assert the same slots, colours, settings and phrase identities survive. This is a preservation test and may already pass; do not manufacture RED or add migration logic.
- [ ] **Step 3: Add a demo contract check.** Implement dependency-free Python unittest reading the named literal `colors` and `ring` arrays from `website/public/demos/loop-station.html`. Test `test_eight_distinct_chromatic_defaults` (eight distinct RGBs, each channel spread at least 64, sixth exactly `#ff0080`) and `test_top_left_clockwise_ring_unchanged` (exact original 12-coordinate reference ring). Fail on missing/ambiguous array markers, not a silent skip. Register `LoopStationDemoContract` with the existing Python interpreter in CMake; do not install packages.
- [ ] **Step 4: Establish RED.** Rebuild and run `LoopSettings`, `LoopPadRender`, and `python3 tests/loop_station/ui/demo_contract_test.py`; record expected default/Pink/demo failures. Run `LoopPersistence` to confirm the preservation baseline.
- [ ] **Step 5: Change only defaults/local display mapping.** Replace slot 5 White with Pink in `defaultTrackSettings`; change only Loop Station's `Colour::Pink` mapping to `RGB(255,0,128)`. Keep White selectable and renderable, keep enum order/IDs and all other colour mappings. Change only `colors[5]` in the demo to `#ff0080`; leave its ring, CSS row order, timing and interaction code unchanged. Document new defaults, saved-colour preservation and corrected physical top-row sidebar in the feature guide.
- [ ] **Step 6: Establish GREEN and commit.** Rebuild, run `LoopSettings`, `LoopPadRender`, `LoopPersistence`, `LoopUi`, the Python demo contract and full `LoopStationTests`; require zero failures. Run `git diff --check` and commit only task files with message `fix: distinguish all eight default loop station rings`.

### Task 3: Integration verification and device handoff

**Files:** create the dated verification report listed above; update plan checkboxes only when supported by evidence. No further feature changes planned.

**Interfaces:** Consume Tasks 1–2's tests, physical coordinate contract and produced firmware sources; no new runtime APIs.

- [ ] **Step 1: Run host verification.** `cmake --build build/tests --target LoopStationTests OledCanvasTests LoopStationOledTests LoopAudioAllocationGuard LoopGestureAllocationGuard LoopHistorySnapshotAllocationGuard -j4`, then `ctest --test-dir build/tests --output-on-failure`. Require all suites including `LoopStationDemoContract` to pass; record exact current counts instead of copying prior counts.
- [ ] **Step 2: Inspect the demo without redesigning it.** Open `website/public/demos/loop-station.html` through a local browser using available browser tools. Confirm unchanged track order and clockwise motion, new sixth swatch, and no browser-console errors. If browser tooling is unavailable, record that check as NOT_RUN; the Python contract is not a browser acceptance claim.
- [ ] **Step 3: Build Release.** Run `DBT_NO_SYNC=1 ./dbt build Release`; require successful ARM compilation/link. Inspect the actual BIN's path, size, SHA-256 and embedded source version; cached filename hashes may be stale. Record warnings transparently and do not change unrelated configuration/toolchain overrides.
- [ ] **Step 4: Review the integrated change.** Obtain independent review according to the retained execution method, focusing on no double inversion, slot/persistence compatibility, partial-row rendering and shared sidebar position. Fix actionable findings, rerun affected tests, and run `git diff --check` before committing evidence.
- [ ] **Step 5: Document and hand off.** Save RED/GREEN, host/build/browser/review evidence and an unchecked hardware list in `docs/superpowers/reports/2026-11-14-loop-station-layout.md`; commit with message `docs: verify loop station layout and palette repair`. Provide the actual BIN path. Hardware checklist: new song top-left Yellow and eight distinct rings; all rings/clear sweeps top-left clockwise; physical press/selection matches track number/settings/clip; top sidebar Start/Stop only; existing song slot/audio/custom White unchanged. Explicitly mark device work NOT_RUN until observed.

## Plan self-review

Spec coverage: Task 1 covers complete physical geometry, input/render/sidebar agreement and unchanged slot identities; Task 2 covers defaults, demo parity and saved/custom-colour policy; Task 3 covers integration/build/review and explicitly separate hardware acceptance. All five review-focus conditions have named tests. Public signatures and persistence contracts are unchanged; the new probe uses production renderer bodies, not an independent renderer implementation. Colour choices and saved-song policy remain proposed assumptions for user review; this plan authorizes no implementation by itself.
