# Measured Loop Length Verification

## Outcome

Implemented the approved recording-length display plan inline on `feature/loop-station-implementation`. No Engine, ResidentAudio, ClockMap, persistence, quantization, or gesture code changed. Captures remain `REC`; empty tracks remain `READY`. Completed phrases use reduced fractions/bars within one sample of the supported musical grids, otherwise measured seconds.

Implementation range: `b94499dd..a1a3ec98`.

- `d604fd84` — pure formatter and direct tests.
- `ca1248bb` — production OLED integration and both probe headers.
- `bda55402` — eliminate embedded-libc dependencies from formatting.
- `a1a3ec98` — preview parity, tests, and user guide.

The user's unrelated staged `.gitignore` remains staged and excluded from all commits. The server was not restarted. Branch kept without merge/push.

## TDD evidence

- Task 1: missing header first, then a minimal empty-output stub caused 4 assertion failures. Implemented the formatter; 5 direct tests / 81 checks passed, covering fractions, triplets, one-sample tolerance, seconds rounding/carry, raw and invalid tempos, uint32 maximum, output truncation, canaries, and size-zero destinations.
- Task 2: successfully compiled the new OLED expectations against the old renderer, producing 8 image mismatches. Integrated the real formatter; production-source probes and OLED comparisons passed. Successive renders reuse one canvas across phrase length changes, capture, clearing with retained frame count, deselection, and loss of model.
- Task 3: new preview tests failed twice on the missing formatter marker. Added BigInt-based JS matching all literal C++ fixtures; 7 preview tests passed using JXA, plus 2 unchanged demo-contract tests. Track strip and OLED share the preview formatter.

## Final verification

Commands were rerun after the final implementation commits:

```bash
python3 tests/loop_station/ui/demo_contract_test.py
python3 tests/loop_station/ui/selection_preview_test.py
cmake --build build/tests --target LoopStationTests OledCanvasTests LoopStationOledTests -j4
ctest --test-dir build/tests --output-on-failure
DBT_NO_SYNC=1 ./dbt build Release
git diff --check
```

Results: demo-contract 2/2; preview 7/7; LoopStationTests 399/399; registered CTest suites 20/20; ARM Release compile/link PASS; diff check clean. Ruff and clang-format hooks pass on task-owned files.

Known existing warnings: extracted probe code has unused `fontNativeHeight` and `wrapIndex`; ARM linker reports an RWX LOAD segment. None was hidden or treated as a new failure.

### ARM linker diagnosis

The initial `std::snprintf` calls in the new standalone header pulled newlib `snprintf` / `_svfprintf_r` and unavailable OS syscall dependencies (`_close`, `_lseek`, `_read`, `_sbrk`, `_write`, `_fstat`, `_isatty`). The linker map traced those references to `loop_station_view.cpp.obj`. Replaced only text assembly with a bounded stack builder and integer `std::to_chars`; the same host tests and the ARM build then passed. No syscall stubs, platform branches, or new libraries were added.

### Firmware artifact

- BIN: `build/Release/deluge-v1_3_0-dev+2026_11_14-43f7427a.bin`
- Size: 1,935,604 bytes.
- SHA-256: `52a3b28c932c9af78d88e47e45b9d883124cc4f664afd28f216f0d4f20a618b4`.
- Embedded version inspected in BIN: `1.3.0-dev-a1a3ec98-dirty`.
- The date/hash in the cached output filename is stale; the embedded version identifies the built implementation. Dirty state was present during build, including unrelated staged `.gitignore` and plan progress edits.
- ELF sizes: text 1,865,024; data 70,544; BSS 402,596.

## Final independent review

Reviewer: `openai/gpt-6.1-sol#max`, session `ses_e216ca077ffeZ3LSfnw8Dpv8Xn`. Reviewed the full implementation range plus spec, plan, rulings, and focused integration risks. Verdict: **Ready to merge: Yes.** No Critical, Important, or Minor defects found. Verified arithmetic bounds, reduced fraction construction, label buffers, shared probes, raw tempo usage, and contained allocation-free ARM workaround.

### Deferred optional coverage

- Add fractional-tempo coverage with a nonintegral sample deadline (the existing 112.5-BPM fixture is exactly 5880 frames).
- Add a near-uint32-maximum input that matches an oversized musical fraction before seconds fallback (current maximum-frame fixture exits before constructing a candidate).

Neither was judged a correctness gap or merge blocker.

## Rulings I made

1. Replace printf-based text assembly with a bounded stack builder and standard integer `to_chars`, instead of platform-specific branches or OS stubs. Reason: host/ARM share the same allocation-free implementation. Cost if wrong: one small private helper needs maintenance.
2. Physical OLED legibility and browser rendering remain NOT_RUN. Reason: available browser tool accepts http(s), not file URLs; the stopped server must remain stopped. Host/JXA/canvas tests are not visual device acceptance. Cost if wrong: later device/browser inspection may require layout corrections.
3. Exclude pre-existing branch changes and staged `.gitignore`. Reason: user requested this display plan only. Cost if wrong: this scoped review does not establish readiness of the entire older branch history.
4. Negative, noninteger, and NaN JS arguments remain outside the uint32 formatter contract. Reason: existing demo callers provide rounded frame counts and a valid tempo. Cost if wrong: future callers accepting invalid values must add validation.

## Outstanding acceptance

Browser visual verification: NOT_RUN. Hardware visual/audio acceptance: NOT_RUN. No claim of device acceptance is made. Suggested device checks: a short synced phrase shows a fraction, an immediate off-grid take shows seconds, tempo changes reclassify labels without changing recorded audio, and the longest input/length pair remains readable.
