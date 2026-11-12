# Loop Station UI Freeze Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Repair the confirmed Community Features menu crash and identify the disabled CROSS-SCREEN fault precisely enough to prescribe and verify its fix.

**Architecture:** Keep the existing runtime settings registry, submenu implementation and entry rules. Register the missing Loop Station menu item and make a registration-count mismatch fail compilation. Treat the disabled shortcut as a separate diagnostic task until its faulting instruction is known; do not claim that the menu repair fixes both reports.

**Tech Stack:** C++23, existing Deluge menu/settings classes, CMake/Ninja, bundled ARM GCC/binutils, existing CppUTest suites.

**Spec:** `docs/superpowers/specs/2026-11-05-loop-station-design.md`.

**Investigation:** `docs/superpowers/reports/2026-11-10-loop-station-ui-freezes.md` contains the report, confirmed Release-image evidence, excluded hypotheses and remaining unknowns.

## Global Constraints

- Keep the default-Off community flag.
- Entry itself never starts recording or transport.
- Keep the CROSS-SCREEN/SONG entry/exit policy from the approved design, including Shift, modal, Arrangement and paired-release behavior.
- Preserve existing runtime feature enum values, XML keys, defaults and display-specific relevance rules.
- Preserve admitted loop audio and existing memory profiles. A disabled entry must not reserve audio memory.
- Do not edit generated `g_english.cpp` or `g_seven_segment.cpp` directly.
- Work in the existing Loop Station worktree. Leave the existing modified plan note and documentation/demo changes outside fix commits.
- Preserve the original failing ELF and BIN before any build overwrites them. Fault addresses are meaningful only with their matching ELF.
- This plan authorizes no speculative popup workaround. The second code repair must be added to this plan after Task 2 identifies its cause.

## Review Focus

- Feature count changes: all declared top-level settings must have real menu registrations; an omitted registration must fail compilation. Owned by Task 1's deliberately failing compile check.
- Menu end navigation: forward/backward traversal and OLED relevance filtering or seven-segment wrapping must never visit a null item. Owned by Task 1's registry inspection and focused menu check.
- Disabled CROSS-SCREEN press/release: retain Song/Session root and playback and perform no loop allocation. Owned by Task 2's clean-boot reproduction and entry-state recording.
- Display presentation: a valid localized hint must complete popup layout, rendering, timer handling and display transfer for the affected screen type. Owned by Task 2's resolved fault instruction and targeted reproduction.
- Repeated actions and other contexts: a second disabled attempt, Shift, active menus and Arrangement must preserve the approved entry rules. Owned by Task 2's focused decision-suite run and post-fix checklist; a helper-only pass cannot substitute for the reported physical failure.

---

## File Structure

- Modify `src/deluge/gui/menu_item/runtime_feature/settings.cpp`: add the missing toggle and registration, infer the array size from its initializer, and check completeness at compile time.
- Read `src/deluge/gui/menu_item/runtime_feature/settings.h`: retain `kNonTopLevelSettings` semantics.
- Read `src/deluge/model/settings/runtime_feature_settings.h/.cpp`: preserve `EnableLoopStation`, its XML name and default.
- Read `src/deluge/gui/menu_item/submenu.cpp`: inspect navigation and display traversal using the repaired table.
- Read `src/deluge/gui/views/session_view.cpp`, `src/deluge/loop_station/geometry.h`, `src/deluge/hid/display/oled.cpp`, and `src/deluge/hid/display/seven_segment.cpp`: follow the actual disabled-entry dispatch rather than only the pure decision helper.
- Read `src/OSLikeStuff/fault_handler/fault_handler.c`: decode the device's existing fault indication.
- Update `docs/superpowers/reports/2026-11-10-loop-station-ui-freezes.md`: record artifact identity, resolved faults, checks and remaining uncertainty.
- Update this plan after Task 2: specify the exact second repair and its regression test once the failing instruction is established.

### Task 1: Complete and guard the Community Features menu

**Files:** Modify `src/deluge/gui/menu_item/runtime_feature/settings.cpp`; update the investigation report.

**Interfaces:**
- Consumes `RuntimeFeatureSettingType::EnableLoopStation`, `RuntimeFeatureSettingType::MaxElement`, `SettingToggle(RuntimeFeatureSettingType)` and `kNonTopLevelSettings`.
- Produces a complete mutable menu-pointer array accepted by the existing `Submenu(..., std::span<MenuItem*>)` constructor. No new public API is needed.

- [x] **Step 1: Preserve the failing artifacts.** Copies are saved under `.superpowers/diagnostics/2026-11-10-loop-station-ui-freezes/failing-release/`; their SHA-256 hashes are recorded in the report. Verify those hashes before using the files for address resolution.

- [x] **Step 2: Write the failing compile-time registration check.** Replace the explicitly sized `subMenuEntries` array with `std::to_array<MenuItem*>({...})`, retaining its current pointers and mutable storage. Add `static_assert(std::tuple_size_v<decltype(subMenuEntries)> == RuntimeFeatureSettingType::MaxElement - kNonTopLevelSettings, "Runtime feature menu registration is incomplete");`. The initializer must determine the size so a missing item cannot be silently padded with null.

- [x] **Step 3: Build to demonstrate the failure.** Run `DBT_NO_SYNC=1 ./dbt build debug`. Expected: compilation fails at the new assertion because 25 registrations do not match 26 top-level settings. A different build failure does not establish the regression check.

- [x] **Step 4: Register the missing feature.** Add `SettingToggle menuEnableLoopStation(RuntimeFeatureSettingType::EnableLoopStation);` alongside the other toggle instances and add `&menuEnableLoopStation` once to the initializer, beside the existing grid-view loop-pad setting. Keep enum and XML values unchanged.

- [x] **Step 5: Build both firmware configurations.** Run `DBT_NO_SYNC=1 ./dbt build debug` and `DBT_NO_SYNC=1 ./dbt build release`. Expected: both exit 0 and the registration assertion passes. Recompute symbol addresses using `arm-none-eabi-nm -S -C`; inspect every pointer of `runtime_feature::subMenuEntries` in each ELF and confirm there are 26 non-null registrations, including the new toggle.

- [ ] **Step 6: Check menu navigation on the affected device when available.** Scroll down to the last visible row and back up. On OLED, include the last page and relevance-filtered rows; on seven-segment, include wrapping. Confirm that Enable Loop Station is reachable, initially Off, and that the existing final setting remains reachable. Record unperformed display variants as NOT_RUN. This is a short reproduction check, not the deferred audio qualification matrix.

- [x] **Step 7: Commit the menu repair.** Stage only `src/deluge/gui/menu_item/runtime_feature/settings.cpp` and the updated investigation report. Commit with `fix: complete loop station feature menu registration`.

### Task 2: Identify the disabled CROSS-SCREEN fault

**Files:** Read the production entry/display/fault files listed above; update the investigation report and this plan. The production fix location is deliberately not selected yet.

**Interfaces:**
- Consumes the preserved failing ELF/BIN and an instruction/return address from the existing fault display or pad indication.
- Produces the resolved failing instruction, the actual entry branch and a concrete second repair with an appropriate regression test. If that evidence is unavailable, produces an explicit unresolved diagnosis rather than an invented code change.

- [ ] **Step 1: Record an independent reproduction.** From a clean boot with `enableLoopStation` Off, use the reported Song/Session layout and press/release CROSS-SCREEN once. Record display type, whether the hint appears before the fault, and the existing fault indication. A readable fault-screen/pad photograph or displayed addresses are sufficient; do not require setting up SysEx logging.

- [ ] **Step 2: Resolve the fault against its matching image.** Decode pad addresses with `fault_handler.c::drawPointer` and `printPointers`, then run the bundled `arm-none-eabi-addr2line -e <matching.elf> -f -C <address>`. Inspect the faulting instruction with `arm-none-eabi-objdump -d`. Record the address, function, source line and nearest callers. Do not use addresses from one build with another build's ELF.

- [ ] **Step 3: Establish the actual failing boundary.** Determine whether the fault occurred before localization, during `displayPopup`, during OLED/seven-segment rendering, or after returning to the UI/timer loop. Confirm whether `Song::ensureLoopStation` was reached; user intent alone does not prove the stored runtime value. If it was reached while the persisted flag is Off, trace that discrepancy first. If it was not reached, keep memory-reservation/audio-engine changes outside the proposed fix.

- [x] **Step 4: Run the existing entry decision checks.** Build `LoopStationTests` with `cmake --build build/tests --config Debug --target LoopStationTests`, then run `build/tests/loop_station/Debug/LoopStationTests -sg LoopView -sn CrossScreenEntryRequiresIdleUnshiftedEnabledSongRoot`. Expected: the helper's enabled/disabled/Shift/modal/Arrangement/release cases pass. Record that this check does not execute the physical popup path.

Task 1 review passed with no findings. The reviewed commit is `ce564ca1`; its Debug/Release builds and ELF pointer inspection passed. Physical Community Features navigation remains NOT_RUN. Task 2's entry-decision test passed (one selected test, six checks), but no fault address/photo or physical reproduction is available here, so the CROSS-SCREEN cause and any second code repair remain unresolved. No speculative popup or allocation change is authorized by the evidence.

- [ ] **Step 5: Extend the plan with the evidence-based second repair.** Name the exact production files, test target, failing input, assertions and red/green commands derived from Steps 1–3. The regression must exercise the failing production boundary; another helper-only decision test is insufficient. Include a short check for repeated disabled press/release and state preservation. Review the added task before implementation, retaining the previously chosen subagent-driven execution method.

## Completion criteria

- The registry defect is repaired and guarded; Debug and Release builds pass.
- The disabled CROSS-SCREEN failure has a resolved cause, its repair is implemented under the extended plan, and a matching regression check passes.
- The user receives an identified replacement `.bin` and concise instructions for the two original interactions. Do not describe a menu-only repair as fixing the shortcut freeze.
- Hardware checks that have not actually been performed remain NOT_RUN. The broad physical audio acceptance matrix remains deferred.

## Plan self-review

The plan covers the confirmed menu bug and the newly reported disabled-entry behavior without assuming a shared cause. Task 1 has a deterministic failing compile check and an exact implementation. Task 2 is a diagnostic checkpoint because the current evidence cannot justify an exact second code change. Its required input, commands and output are explicit. The five review conditions are assigned above. No firmware production file was changed while writing this plan.
