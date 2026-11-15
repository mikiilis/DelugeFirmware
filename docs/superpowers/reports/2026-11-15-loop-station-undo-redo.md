# Loop Station layer undo and redo — documentation and verification

## Outcome

Documented and host-verified the approved selected-track layer undo/redo feature on
`feature/loop-station-implementation`, up to `d854facc` (Tasks 1–6). Both feature guides now describe the
confirmed two-press shortcuts, layer/empty semantics, cancellation and fenced history, and save/load
behaviour; the misleading one-press undo text is removed. No production or test code changed in this
task; no schema or memory-profile change is introduced. The final whole-branch review had not been run
when this report was written.

## Documentation changes

- `docs/features/loop_station.md`
- `website/src/content/docs/features/loop_station.mdx`

Both now state: one capture gate session is one layer over multiple passes; **BACK** then released
**BACK** confirms Undo and **SHIFT+BACK** twice confirms Redo, with no rapid-double-click requirement and
no time limit; the first press leaves active capture running and confirmation closes/removes only the
targeted take; a pre-known incomplete-history/NoMemory rejection leaves capture open while a later
failure preserves cumulative audio, leaves capture closed and reports honestly; an unchanged active
overdub closes with no older-layer fallthrough; Undo to empty keeps settings/binding/output/FX and initial
Redo restores stopped; ordinary history preserves playback/phase; new edits discard the Redo branch and
explicit Clear is not undoable; history is bounded/fenced and cannot skip a missing layer; the prompt is
canceled by unrelated controls (including tempo), pads/selection, transport or a model/target change,
while SHIFT or a release alone preserve it; an accepted job continues across a harmless UI handoff and
publishes the committed result; OLED uses persistent rows and seven-segment a persistent view-owned
scrolling message; saving an undone-empty track omits its old checkpoint and volatile Redo survives Save
without becoming durable; a loaded checkpoint is one volatile base recording with no old layers; and a
live snapshot taken before navigation stays readable through a bounded owned reference. Browser-demo
limitations are unchanged.

## Host verification (fresh)

Bundled toolchain PATH was used throughout; raw logs are in
`.superpowers/sdd/2026-11-15-loop-station-undo-redo/verification/`.

```bash
cmake --build build/tests -j4
ctest --test-dir build/tests --output-on-failure
python3 tests/loop_station/ui/selection_preview_test.py
git diff --check
```

- Host build: PASS (exit 0).
- Registered suites: **27/27 pass** (11.52 s), including all allocation guards, production probes,
  `LoopStationTests`, `LoopStationDemoContract`, OLED suites and `LoopFatFsAllocationTests`.
- `LoopStationTests`: **499 tests, 499 ran, 3,907,835 checks, 0 ignored, 0 filtered**.
- `selection_preview_test.py`: **7/7 pass**.
- `git diff --check` (including staged): clean.

## Sanitizer verification (separate configuration)

Configured with the bundled tools and the already-present `build/tests/_deps/*-src` sources, entirely
offline (`FETCHCONTENT_FULLY_DISCONNECTED=ON`); nothing was installed or downloaded:

```bash
cmake -S tests -B build/tests-loop-ubsan -G "Ninja Multi-Config" -DLOOP_STATION_UBSAN=ON \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_*=build/tests/_deps/*-src
cmake --build build/tests-loop-ubsan --config Debug \
  --target LoopStationTests LoopPageAllocationGuard LoopAudioAllocationGuard \
           LoopGestureAllocationGuard LoopHistorySnapshotAllocationGuard LoopFatFsAllocationTests -j4
ctest --test-dir build/tests-loop-ubsan -C Debug -R '<the six targets>' --output-on-failure
```

Result: build PASS; **6/6 tests pass** (9.68 s); UBSan-instrumented `LoopStationTests` reports 499/499
tests and 3,907,835 checks with no sanitizer report.

## Firmware build

`DBT_NO_SYNC=1 ./dbt build Debug` compiled all 318 steps, including every new loop-station and
`loop_station_view.cpp` object, but the **final link failed** with undefined newlib syscall stubs
(`_close`, `_fstat`, `_isatty`, `_lseek`, `_read`, `_sbrk`, `_write`). No Debug artifact exists.

This is a pre-existing, feature-independent blocker: a detached temp worktree at the pre-implementation
commit `4671c801` reproduces the identical Debug link failure (same seven symbols), and object
inspection shows no object references the syscalls directly — the pull is newlib stdio reached through
libstdc++'s exception/terminate runtime, which many baseline objects already use. The Debug and Release
links differ only by `-g` (and `-DNDEBUG` at compile), and the arm-none-eabi debug C library variant lacks
the syscall/retarget objects. No linker configuration was modified.

Supplementary `DBT_NO_SYNC=1 ./dbt build Release` links successfully, confirming the feature object set is
link-clean under the project's working configuration:

- BIN `build/Release/deluge-v1_3_0-dev+2026_11_14-43f7427a.bin`, 1,944,940 bytes, sha256
  `a641159eb072ff9fb2bb028caa9045b4b625994610baeb72185a4f94f214a757`; embedded version
  `1.3.0-dev-d854facc-dirty`; ELF text 1,874,080, data 70,832, BSS 402,700. Not flashed.

## Hardware acceptance

All hardware checks are **NOT_RUN** (no hardware available): audible layer removal/restoration with no
clicks or phase error, capture-boundary behaviour, OLED/seven-segment prompt readability and ergonomics,
BACK LED behaviour, and real memory/DSP/callback deadlines, including delayed completion latency. Host
probes and the Release link are not hardware validation.

## Outstanding acceptance

Physical Deluge verification remains **NOT_RUN**. The controller's final whole-branch review (Sol xhigh)
has not yet run; deferred Minor items from Tasks 1–6 remain for that triage.
