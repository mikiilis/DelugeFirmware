# Loop Station UI freeze investigation

Date: 2026-11-10.

Worktree: `.worktrees/loop-station-sdd`, branch `feature/loop-station-implementation`.

Inspected firmware: `build/Release/deluge.elf`, produced by the previous successful `DBT_NO_SYNC=1 ./dbt build`, version `v1.3.0-dev-ba3033ee-dirty`.

Copies of the failing ELF/BIN are preserved locally under `.superpowers/diagnostics/2026-11-10-loop-station-ui-freezes/failing-release/`, outside the build outputs. SHA-256:

- `deluge.elf`: `59234fd2c6be60e6e0d1f162c98141ea16f98a19bd4d3f2849fd503599bebed6`.
- `deluge.bin`: `dc8f79a42d29bc986623ab371cdffb5cd640b10a576f6b7acec06e72b532b021`.

## Reported behavior

1. Scrolling toward the end of Settings → Community Features freezes the device.
2. Pressing CROSS-SCREEN in Song View also freezes the device and displays the colored pad fault indication.
3. The user confirmed that the CROSS-SCREEN failure occurred on the first attempt, without having enabled Loop Station.

The exact fault addresses and display variant have not been captured. No hardware fault reproduction was performed during this investigation.

## Confirmed defect: incomplete community menu registration

`src/deluge/model/settings/runtime_feature_settings.h` declares 26 feature settings before `MaxElement`. The new `EnableLoopStation` value is last. Its metadata is initialized in `RuntimeFeatureSettings::init()` with the XML key `enableLoopStation` and default `Off`.

`src/deluge/gui/menu_item/runtime_feature/settings.cpp` declares `subMenuEntries` with the size `RuntimeFeatureSettingType::MaxElement - kNonTopLevelSettings`. With `kNonTopLevelSettings == 0`, that is 26 entries. The initializer contains only 25 pointers: the `menuEnableLoopStation` instance and its registration were omitted. C++ zero-initializes the final pointer.

The production submenu implementation dereferences item pointers while scanning relevance, drawing OLED rows and moving the selection. Consequently, approaching the final entry can dereference null before the missing row is selected.

This was also checked in the actual Release ELF:

- `deluge::gui::menu_item::runtime_feature::subMenuEntries`: address `0x0c0aa348`, size `0x68` (26 ARM pointers).
- The pointer at `0x0c0aa3ac`, entry 25 using zero-based indexing, is `0x00000000`.

These addresses identify this artifact only. Rebuilds require resolving the symbols again.

## Disabled CROSS-SCREEN: branch identified, cause unconfirmed

`SessionView::buttonAction()` passes the runtime flag to `crossScreenEntryDecision()`. With the feature disabled, the expected branch is `EntryDecision::FeatureDisabled`. It calls:

```cpp
display->displayPopup(l10n::get(l10n::String::STRING_FOR_COMMUNITY_FEATURE_LOOP_STATION));
```

It returns without calling `Song::ensureLoopStation(true)`, reserving loop audio memory, changing root UI, or starting transport.

Both generated language sources contain the feature label. The Release ELF's English map entry for string index 513 is present, has length 19 and points to the null-terminated text `Enable Loop Station`. An absent English translation is therefore not supported by the inspected evidence.

The Release disassembly of `SessionView::buttonAction()` likewise contains the disabled branch and its popup dispatch. Its local stack allocation is 68 bytes plus saved registers; a very large entry-function stack frame is not supported by this inspection.

This does not establish that the popup, its drawing, its timer, or its transfer to the physical display completes successfully. It also does not prove that the runtime flag value matched the user's expectation at the failing instruction. The fault location is still required before prescribing a second code fix.

## Existing coverage gap

`tests/loop_station/view_tests.cpp` checks the pure entry-decision helper. `menu_tests.cpp` checks the Loop Station settings model. Neither executes the actual Community Features registry traversal or the disabled shortcut's physical popup path. The previous successful host tests and firmware links did not cover these two device interactions.

## Required behavior

- Every top-level runtime feature has a registered, non-null menu item. Loop Station is exposed exactly once and remains Off by default.
- Scrolling in either direction through Community Features is safe on OLED and seven-segment displays, including relevance filtering and end behavior.
- Unshifted CROSS-SCREEN at an idle Song/Session root with Loop Station Off shows the enable hint and retains the current root, playback and audio ownership.
- A disabled shortcut press and release, including repeated attempts, never starts allocation or enters Loop Station.
- Shifted, modal and Arrangement contexts retain the behavior specified in the original Loop Station design.

## Next evidence for the shortcut failure

Preserve the failing `.elf` and its matching `.bin`. On a clean boot, reproduce the disabled shortcut once and capture the existing fault indication. The built-in fault handler already encodes instruction/return addresses on the pads; a readable photograph or a displayed address is sufficient. A SysEx log setup and a long audio qualification run are not required for this fault capture.

Decode the indication using `src/OSLikeStuff/fault_handler/fault_handler.c`, then resolve addresses against the preserved ELF using the bundled `arm-none-eabi-addr2line -e <matching.elf> -f -C <address>` and inspect the corresponding disassembly. Record the resolved instruction and call path before changing popup or entry behavior.

## Task 1 completion: Community Features menu registration

Replaced the fixed-size `subMenuEntries` array with a deduced `std::to_array<MenuItem*>` and a compile-time count assertion. The expected pre-fix build failed at that assertion with `(25 == 26)`. Registered `menuEnableLoopStation` exactly once after the grid-view loop-pad toggle.

Debug and Release firmware builds both passed. `arm-none-eabi-nm -S -C` and ELF data inspection confirmed 26 non-null pointers in each array; entry 19 resolves to `menuEnableLoopStation`. Full pointer evidence is in `.superpowers/sdd/2026-11-10-loop-station-ui-freezes/task-1-report.md`.

Hardware navigation was NOT_RUN because no device was accessible from this workspace. OLED last-page/relevance-filtering and seven-segment wrapping remain unverified on device.

## Task 2 local check: disabled CROSS-SCREEN entry

Ran `cmake --build build/tests --config Debug --target LoopStationTests` and then `build/tests/loop_station/Debug/LoopStationTests -sg LoopView -sn CrossScreenEntryRequiresIdleUnshiftedEnabledSongRoot`. The selected test passed (1 test, 6 checks); the built suite reports 286 total tests. This validates the pure entry-decision helper only and does not reproduce or exercise the physical display popup path.

The device fault could not be reproduced in this workspace, and no fault-screen photo or decoded instruction/return address is available. Therefore, the CROSS-SCREEN fault location and cause remain unresolved. No second production change is justified yet; resolve a captured address against the preserved failing ELF before changing popup or entry behavior.

## Follow-up investigation: production rendering and firmware startup

The menu defect is a definite null dereference in submenu traversal. The disabled shortcut never traverses `runtime_feature::subMenuEntries`; a shared cause has not been established. No additional production source was changed during this follow-up.

### Production popup probe

A temporary host probe extracted unchanged production bodies for OLED line layout, popup setup, glyph drawing, canvas clipping and popup/background composition. It linked the actual firmware font data. It exercised five labels, rounded corners On and Off, for 100,000 total drawing/composition cycles under UndefinedBehaviorSanitizer, with exit 0 and no sanitizer diagnostics.

For the exact `Enable Loop Station` label, the production layout produces one 114-pixel line and a popup from `(1,13)` to `(127,34)` on the 128×48 display. Text and box fit the canvas; the code sets the normal 800 ms popup timeout. This input does not reach long-word wrapping or the eight-line limit. The paired button release has no CROSS-SCREEN action in the traced Song-root event path.

This probe does not execute the complete firmware button handler, virtual display dispatch, actual timer callbacks, ARM instructions, interrupts or physical SPI/DMA transfer. It rules out the proposed text/layout overflow for this exact input, not every device runtime failure. ASan was unavailable: even a trivial ASan executable failed before `main`, so no ASan pass is claimed.

Detailed source references, extraction ranges and commands are saved in `.superpowers/diagnostics/2026-11-10-loop-station-ui-freezes/popup-investigation.md`. The generator and generated probe source are preserved in its `popup-probe/` subdirectory; the generator writes build products to `/tmp/loop-popup-probe/`.

### Startup, memory and setting checks

- All four relocated SDRAM sections in the preserved failing ELF match the BIN at their expected load offsets. Its startup instructions copy the matching ranges before constructors run. The packed load image ends `0xc4294` bytes below the program stack; there is no static image/stack overlap. This does not measure runtime stack usage.
- All 69 constructor-table entries point into executable code. The new LoopStationView initializer occurs once and writes only its 24-byte object; it performs no loop-memory allocation.
- `EnableLoopStation` is index 25 in a 26-element settings array, and initialization explicitly sets it Off. XML persistence uses names, so appending its enum did not shift older setting values.
- With no loop owner, archive, retirement, clock and audio-adapter integration paths return before accessing loop arenas. Added scheduler registrations remain below the 25-task capacity. The hardware feasibility harness is disabled in the build cache and excluded from Release by CMake.
- Inspected ARM stack frames are small for the two entry functions: 68 bytes in `SessionView::buttonAction` and 324 bytes in `Song::ensureLoopStation`, excluding saved registers and callees. No large entry-local object was identified.
- A candidate ARM alignment issue in owner initialization was checked against `MemoryRegion::setup`, `padSize` and allocation splitting. These preserve eight-byte payload alignment, sufficient for the inspected aligned NEON store; no alignment defect was established.

Details and artifact-specific section addresses are saved in `.superpowers/diagnostics/2026-11-10-loop-station-ui-freezes/init-memory-investigation.md`.

The actual runtime flag remains unobserved. Reading an SD-card setting `enableLoopStation=1` overrides the Off default, and historical Loop Station commits used the same XML key. This is a possible explanation for reaching enabled setup without a new menu interaction, not evidence that this happened on the user's device. The existing pasted audio log and local capture files contain no fault PC/LR addresses that distinguish these branches.

### Matching image for either installed version

The menu-fixed Release from commit `ce564ca1` is also preserved under `.superpowers/diagnostics/2026-11-10-loop-station-ui-freezes/menu-fix-release/`, in addition to the original `failing-release/` pair:

- Menu-fixed ELF SHA-256: `c83bedbf884851ec2ced5de27d88f0fd803dc7faa84e286832e48ed030da84d1`.
- Menu-fixed BIN SHA-256: `7596ff2d6c79e24615f97c0c03ac11d3170d82277f13656bb213dec31161b6bf`.

A photo of the existing fault pads plus the installed firmware version can now be resolved against either preserved build. That is the missing evidence needed to locate the failing instruction; another broad logging or audio qualification run is not required.
