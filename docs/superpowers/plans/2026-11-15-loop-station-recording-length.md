# Loop Station Recording Length Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Display short completed recordings as meaningful musical fractions when aligned, or measured seconds otherwise, instead of forcing every recording to at least `1 BAR`.

**Architecture:** Introduce a pure header-only formatter that consumes recorded frames and raw Q16 tempo, with integer-only recognition of straight/triplet musical lengths. Keep recording-state precedence in the view, reuse the formatter in both production-source test probes, and mirror the contract in the HTML demo.

**Tech Stack:** C++23, CppUTest, existing real-canvas probes, Python unittest with Node/JXA, CMake/CTest, DBT ARM Release toolchain.

**Spec:** `docs/superpowers/specs/2026-11-15-loop-station-recording-length-design.md` (proposed design; review its formatting choices before execution).

## Global Constraints

- Display formatting only: preserve captured samples, playback, start/stop quantization, and all saved data.
- Preserve the current three-line OLED layout, title, track number, input, BPM, and right-aligned length field. No separator or new screen controls.
- During initial recording or overdub, keep `REC`. An empty track stays `READY`. Live elapsed recording time is out of scope.
- Recognize lengths that are a positive integer multiple of either 1/32 bar or 1/48 bar, within one audio sample of their expected duration; check 1/32 first, then 1/48, and reduce fractions using GCD.
- Exact examples: `1/16 BAR`, `3/16 BAR`, `3/2 BARS`, `1 BAR`, `4 BARS`, `0.37 s`, `<0.01 s`. Every final length label is at most 8 ASCII characters.
- Seconds use 44100 samples/second, half-up hundredths; positive durations below the display resolution show `<0.01 s`. At 1000 rounded seconds and above, use nearest whole seconds.
- Musical labels describe duration at the current tempo, not the tempo at capture. Seconds always derive from sample count.
- No phrase trimming, padding, resampling, changed quantization, persistence migration, new dependencies, or firmware selection-gesture changes.
- Keep `colors`, `ring`, pad animation, demo selection toggling, and the demo's illustrative minimum recording duration unchanged. Do not restart the stopped HTTP server.
- Preserve unrelated working-tree changes, including the staged `.gitignore` change present when this plan was written. Commit only task-owned paths.

## Review Focus

- Rounded sample deadlines: ceiling rounding must still recognize fractions at fractional tempos; two samples outside the defined tolerance must not silently snap.
- Free and tiny takes: off-grid durations must show measured seconds; a nonempty take must never be labelled `0 BAR` or `0.00 s`.
- Tempo edits and settings: use raw current Q16 tempo, not rounded BPM or start/stop mode, and do not mutate the phrase while classifying it.
- Pixel width and successive renders: `IN: BALANCED` plus every supported length must fit; shorter labels, deselection, and loss of model must erase previous pixels.
- Arithmetic and API boundaries: uint32 frame maximum, invalid tempo, zero-length buffers, and truncated buffers must not overflow, divide by zero, or write past the destination.

---

## Files and responsibilities

- Create `src/deluge/loop_station/length_display.h`: pure bounded formatter, no engine/UI dependencies.
- Create `tests/loop_station/length_display_tests.cpp`; modify `tests/loop_station/CMakeLists.txt`: direct formatter unit tests registered in the existing `LoopStationTests` target.
- Modify `src/deluge/gui/views/loop_station_view.cpp`: replace the old whole-bar formatter with capture/empty handling plus the new formatter.
- Modify `tests/oled_canvas/probe_support.h` and `tests/loop_station/ui/probe_support.h`: make the production formatter visible to extracted view helpers.
- Modify `tests/oled_canvas/loop_station_oled_tests.cpp`: new real-canvas image/width/transition expectations.
- Modify `website/public/demos/loop-station.html` and `tests/loop_station/ui/selection_preview_test.py`: preview parity and dependency-free regression tests.
- Modify `website/src/content/docs/features/loop_station.mdx`: explain fractions, seconds, current-tempo interpretation, and unchanged quantization.

## Execution prerequisites

No implementation is authorized by creation of this plan. Review the companion proposed design, especially the one-sample tolerance, supported grids, seconds precision, and continued `REC` during capture.

Retain the previously selected execution preference unless the user changes it: subagent-driven; implementers `opencode/deepseek-v4.1-flash-free#high`, task reviewers `opencode/deepseek-v4.1-flash-free#max`, final review `openai/gpt-6.1-sol#max`. Recheck model availability and do not silently substitute. Verify the feature checkout/worktree before execution.

Run commands from the repository root with:

```bash
export PATH="$PWD/toolchain/current/python/bin:$PWD/toolchain/current/cmake/bin:$PWD/toolchain/current/clang-format:$PATH"
```

Use the existing `build/tests` CMake build. If unconfigured, use `cmake -S tests -B build/tests` with the project's documented host-test dependencies. Do not install new dependencies implicitly.

### Task 1: Implement the pure measured-length formatter

**Files:** Create `src/deluge/loop_station/length_display.h`, create `tests/loop_station/length_display_tests.cpp`, modify `tests/loop_station/CMakeLists.txt`.

**Interfaces:**
- Consumes: `FrameIndex`, `kSampleRate`, raw `ClockMap::tempoQ16()` representation; no ClockMap object or Engine state required.
- Produces: `inline void loop_station::formatPhraseLength(FrameIndex frames, uint32_t tempoQ16, char* out, size_t size)`. A nonzero-sized valid destination receives a bounded NUL-terminated label; `size == 0` performs no access, including for `out == nullptr`.

- [ ] **Step 1: Add failing table-driven tests.** Register the new test file in `LoopStationTests`. Include the proposed header and add `TEST_GROUP(LoopLengthDisplay)` with a helper that calls the real formatter and `STRCMP_EQUAL`s the expected output. Pin these exact cases at `120U * 65536U`:

```cpp
struct LengthCase { FrameIndex frames; const char* expected; };
const LengthCase cases[] = {
    {0, "0.00 s"}, {1, "<0.01 s"}, {220, "<0.01 s"}, {221, "0.01 s"},
    {1838, "1/48 BAR"}, {2757, "1/32 BAR"}, {3675, "1/24 BAR"},
    {5513, "1/16 BAR"}, {7350, "1/12 BAR"}, {11025, "1/8 BAR"},
    {14700, "1/6 BAR"}, {16538, "3/16 BAR"}, {22050, "1/4 BAR"},
    {29400, "1/3 BAR"}, {44100, "1/2 BAR"}, {66150, "3/4 BAR"},
    {88200, "1 BAR"}, {132300, "3/2 BARS"}, {352800, "4 BARS"},
    {16317, "0.37 s"}, {5511, "0.12 s"}, {5512, "1/16 BAR"},
    {5514, "1/16 BAR"}, {5515, "0.13 s"},
    {330750, "7.50 s"}, // 15/4 BARS exceeds 8 characters
};
for (const auto& c : cases) {
    char out[24];
    formatPhraseLength(c.frames, 120U * 65536U, out, sizeof(out));
    STRCMP_EQUAL(c.expected, out);
    CHECK(std::strlen(out) <= 8);
}
```

Add independent tests with these assertions (same real helper):

```cpp
// fractionalTempoAndTempoChangesUseRawQ16
expect(5880, 112U * 65536U + 32768U, "1/16 BAR");
expect(5513, 121U * 65536U, "0.13 s");
expect(16317, 60U * 65536U, "0.37 s");
expect(16317, 120U * 65536U, "0.37 s");

// secondsCarryAndLargeInputsAreBounded (invalid tempo selects seconds)
expect(439, 0, "0.01 s");
expect(44099, 0, "1.00 s");
expect(44099779, 0, "999.99 s");
expect(44099780, 0, "1000 s");
expect(UINT32_MAX, 0, "97392 s");
expect(UINT32_MAX, 1000U * 65536U, "97392 s");
expect(16317, 1001U * 65536U, "0.37 s");
expect(16317, 1U, "0.37 s");
```

Define `expect(FrameIndex,uint32_t,const char*)` locally in the test file to compare a 24-byte buffer against the given string and assert `strlen <= 8`. For destination safety, surround a 4-byte destination with canaries, expect `1/1` plus NUL for `(5513,120<<16)`, preserve canaries; with a 1-byte destination expect only NUL; call `(1,0,nullptr,0)` and require no crash. Do not treat truncation as a complete UI label; production uses a 24-byte destination.

- [ ] **Step 2: Establish RED.** Run `cmake --build build/tests --target LoopStationTests -j4`; expect a missing `length_display.h` failure. Introduce the header with the declared signature and a minimal stub that writes an empty string when `size > 0`, rebuild, and run `build/tests/loop_station/LoopStationTests -sg LoopLengthDisplay`. Require genuine failed output assertions before implementing the algorithm; compilation alone is not evidence for the arithmetic tests.

- [ ] **Step 3: Implement the formatter in the new header.** Include only standard integer/string formatting/GCD utilities and `config.h`. Keep any helper functions/constants inline in a feature-specific detail namespace. No heap allocation or floating point.

Use this algorithm to avoid misclassification from a repeatedly rounded `framesPerBar()` value:

```text
C = uint64_t(kSampleRate) * 240 * 65536  // exact frames * Q16 BPM per bar
For D in [32, 48], if 0 < tempoQ16 <= 1000*65536 and frames > 0:
    scaled = uint64_t(frames) * tempoQ16 * D
    N = (scaled + C/2) / C
    if N == 0: continue
    expectedFrames = ceil(N*C / (uint64_t(tempoQ16)*D))
    if abs(frames - expectedFrames) > 1: continue
    reduce N/D using GCD
    form whole bars or fractional bars with the spec's singular/plural rule
    if full label length <= 8: return that label
    otherwise return the seconds fallback
Seconds fallback:
    hundredths = (uint64_t(frames)*100 + kSampleRate/2) / kSampleRate
    positive frames with zero hundredths -> "<0.01 s"
    hundredths < 100000 -> integer part + two-digit fractional part + " s"
    otherwise -> ((uint64_t(frames)+kSampleRate/2)/kSampleRate) + " s"
```

Use a local temporary large enough for the full candidate label before checking length, then copy into the caller's bounded destination. Do not inspect the length of an already truncated candidate. For unsigned distance, subtract smaller from larger. Calculate ceiling by quotient/remainder (not floating point). The largest products for uint32 frames, Q16 BPM <=1000, and D<=48 fit uint64; retain the casts before multiplication and test the maximum case.

- [ ] **Step 4: Establish GREEN.** Run `cmake --build build/tests --target LoopStationTests -j4` and `build/tests/loop_station/LoopStationTests -sg LoopLengthDisplay`; require zero failed tests. Run the full `LoopStationTests` once before committing.

- [ ] **Step 5: Commit only this task.** Run `git diff --check`, stage the three named files explicitly, and commit `feat: format measured loop lengths as fractions or seconds`.

### Task 2: Integrate measured lengths into the OLED and both probes

**Files:** Modify `src/deluge/gui/views/loop_station_view.cpp`, `tests/oled_canvas/probe_support.h`, `tests/loop_station/ui/probe_support.h`, and `tests/oled_canvas/loop_station_oled_tests.cpp`.

**Interfaces:**
- Consumes: Task 1's `formatPhraseLength(FrameIndex,uint32_t,char*,size_t)`, `SlotState`, `ClockMap::tempoQ16()`, and existing Canvas primitives.
- Produces: view-local `void formatTrackLength(const SlotState& state, uint32_t tempoQ16, char* out, size_t size)`, plus the updated `renderOLED` call. Capture precedes empty; only a completed phrase delegates to Task 1.

- [ ] **Step 1: Write failing OLED expectations.** Replace `testBarCountRoundsToNearestAndNeverShowsZero` with `testCompletedPhraseUsesMeasuredLength` using the cases below. Set `model.testClock.bpmQ16 = 120U*65536U`; do not fake the old `framesPerBarValue` to control this formatter.

```cpp
// Selected slot 0, hasPhrase true: expectMainScreen(..., "TRACK 1", "120 BPM", "IN: LEFT", expected)
// frames -> expected, using the existing expected-image composer:
// 1 -> "<0.01 s"; 5513 -> "1/16 BAR"; 16317 -> "0.37 s";
// 132300 -> "3/2 BARS"; 352800 -> "4 BARS".
```

Update `testEmptyCaptureAndPhraseLabels` to use 352800 frames for `4 BARS`; keep initial recording and overdub assertions expecting `REC`. When clearing `hasPhrase`, deliberately leave `phraseFrames = 352800` and require `READY`.

Add `testCurrentTempoReclassifiesLength` rendering 5513 frames at 120 BPM as `1/16 BAR`, then at 121 BPM as `0.13 s`; keep the phrase frames unchanged. Add a fractional-BPM image for 5880 frames at 112.5 BPM: row 2 `113 BPM`, row 3 `1/16 BAR`.

Add `testLengthTransitionsClearPreviousPixels`: initialize one actual Canvas once, then render on it successively: selected `4 BARS` -> `0.37 s` -> `REC` -> `READY` -> invalid selection -> `currentSong == nullptr`. Compare each full image to a newly composed expected image and check canaries after every render; never clear the actual Canvas between calls.

Extend the width test with every unique label from Task 1's tables plus `READY`, `REC`, `999.99 s`, and `97392 s`: require `width("IN: BALANCED") + width(label) < 128`. Preserve title, BPM, settings-menu, and copy-confirmation tests.

- [ ] **Step 2: Establish RED.** Build/run `LoopStationOledTests`; require successful compilation followed by mismatches for the short/off-grid labels against the old `1 BAR` formatter. Record those failures.

- [ ] **Step 3: Integrate, without copying the algorithm.** Include `deluge/loop_station/length_display.h` in the production view and both probe-support headers. Replace `formatBarCount` with the exact `formatTrackLength` signature from this task's Interfaces; emit `REC`, then `READY`, otherwise delegate. Pass `m->clock().tempoQ16()` instead of `framesPerBar()` from `renderOLED`. Leave helper extraction markers and all other rendering code unchanged. Both probe generators already extract the helpers and will see the new header via their support headers; do not edit generated sources or duplicate the implementation in stubs.

- [ ] **Step 4: Establish GREEN across both probes.** Run:

```bash
cmake --build build/tests --target LoopStationTests OledCanvasTests LoopStationOledTests -j4
ctest --test-dir build/tests -R '^(LoopStationTests|OledCanvasTests|LoopStationOledTests)$' --output-on-failure
```

Require all three suites to pass, including settings and existing start/stop timing tests. Review the diff to confirm no Engine, ResidentAudio, ClockMap, persistence, gesture, or quantization changes.

- [ ] **Step 5: Commit.** Run `git diff --check`, stage only the four named files, commit `feat: show measured recording lengths on the loop station OLED`.

### Task 3: Match the preview, document the labels, and verify the integrated change

**Files:** Modify `website/public/demos/loop-station.html`, `tests/loop_station/ui/selection_preview_test.py`, `website/src/content/docs/features/loop_station.mdx`.

**Interfaces:**
- Consumes: Task 1's formatting contract and Task 2's unchanged three-line screen.
- Produces: pure JS `formatPhraseLength(frames, tempoQ16)` returning the same labels; existing `barsText(track)` delegates after preserving empty/capture states. Demo `track.length` is seconds, converted with `Math.round(track.length * 44100)`; pass `Math.round(tempoBPM * 65536)`.

- [ ] **Step 1: Add failing preview tests.** Replace `test_bars_text_reports_capture_and_rounded_bars` with `test_bars_text_reports_measured_length`, extracting `barsText` and the new formatter via the existing `run_demo_helpers` API. Supply `tempoBPM = 120`. Assert:

```python
# barsText states/lengths in seconds:
# empty/0 -> READY; recording/0.37 -> REC; overdubbing/0.37 -> REC;
# playing/0.125 -> 1/16 BAR; stopped/0.37 -> 0.37 s;
# playing/2 -> 1 BAR; playing/3 -> 3/2 BARS; playing/8 -> 4 BARS.
```

Add `test_length_formatter_matches_firmware_contract` with the complete literal sample/tempo/label tables from Task 1 (including tolerance, fractional tempo, zero/invalid tempo, and maximum-frame cases). Keep existing deselection, global-settings, and pulse tests intact. Use actual extracted JS helpers, not a Python reimplementation as the oracle.

- [ ] **Step 2: Establish RED.** Run `python3 tests/loop_station/ui/selection_preview_test.py`; require the missing formatter marker or old whole-bar output to fail the new assertions.

- [ ] **Step 3: Update the preview.** Implement JS `formatPhraseLength` with the same integer/rational rules as Task 1. Use BigInt for intermediate arithmetic so the large-frame parity cases do not lose precision in JavaScript Number multiplication; convert the measured frame count and Q16 tempo to BigInt before multiplying. Return strings only. Use integer quotient/remainder, local Euclidean GCD, and the same 8-character fallback. Preserve the public `barsText(track)` name and capture/empty handling. Reuse `barsText` for completed track-strip length text so the strip does not contradict the OLED; retain the existing state prefix and `live` indication for initial capture. Do not change recording simulation, track defaults, colors, rings, or selection behavior. The planning environment has no Node executable, but JXA reports `typeof BigInt == "function"`; preserve the test runner's existing Node/JXA fallback, do not install Node or silently skip the new cases here.

- [ ] **Step 4: Document the behavior.** In the existing feature guide, explain completed-phrase fractions/seconds with `1/16 BAR` and `0.37 s` examples; `REC` during recording/overdub; one-sample recognition tolerance; current-tempo interpretation; and that no recorded audio is rounded, trimmed, or padded by this display change. Distinguish existing synced Start/Stop scheduling from nearest-size post-recording rounding.

- [ ] **Step 5: Establish GREEN and run integration verification.** Run:

```bash
python3 tests/loop_station/ui/demo_contract_test.py
python3 tests/loop_station/ui/selection_preview_test.py
cmake --build build/tests --target LoopStationTests OledCanvasTests LoopStationOledTests -j4
ctest --test-dir build/tests --output-on-failure
DBT_NO_SYNC=1 ./dbt build Release
git diff --check
```

Require both Python suites, every registered CTest suite, and ARM compile/link to pass. Record actual counts and any pre-existing warnings; do not promise warning-free output. Use a direct `file://` browser preview if tooling supports it; otherwise mark browser verification NOT_RUN. Never restart the stopped HTTP server. Device checks remain NOT_RUN until observed.

- [ ] **Step 6: Commit and obtain final review.** Stage only this task's three files and commit `docs: match loop length preview and explain measured durations`. Obtain the agreed independent whole-change review, focused on arithmetic, no audio mutation, the two extraction probes, JS parity, and row width. Report the actual BIN path and build evidence if execution produces a firmware artifact; a stale hashed filename is not proof of the embedded version. Keep the branch without merge/push unless the user requests integration.

## Self-review

- Spec coverage: Task 1 implements every label, precision, tolerance, length-limit, and arithmetic rule; Task 2 pins real OLED behavior and unchanged capture/empty handling; Task 3 supplies preview parity and user documentation. Recording quantization and live duration are explicit non-goals.
- Step scan: Tasks carry concrete signatures, input/output fixtures, RED/GREEN gates, and named commit scopes. No production function bodies are prescribed where signatures and tests suffice; the rational algorithm is stated because rounded frames-per-bar would produce different results.
- Type consistency: formatter accepts raw uint32 Q16 tempo, not FrameIndex frames-per-bar. The view wrapper and both test probes use the same header. JS uses BigInt internally but returns strings compatible with the existing preview.
- Review Focus: Task 1 covers tolerance, tiny/free takes, fractional/current tempo, invalid inputs, uint32 limits, and output buffers; Task 2 covers source-label width and successive renders. Task 3 duplicates fixtures across runtimes to pin parity.
- Proportion: three testable deliverables: pure formatting, actual screen integration, preview/documentation. No engine rewrite, new clock accessors, migrations, or new preferences are introduced.
