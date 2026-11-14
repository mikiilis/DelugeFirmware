# Loop Station Main Screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Loop Station OLED status rows with the three-line screen previewed in the interactive demo: centred `LOOP STATION`, selected track plus tempo, then input source plus bar count.

**Architecture:** Add two read-only `ClockMap` accessors for display, then rewrite `LoopStationView::renderOLED` around the production canvas alignment helpers. Extend the existing OLED probe — which compiles unmodified production source — with those canvas helpers and lightweight clock/engine stubs, so image tests exercise the real renderer rather than a second implementation.

**Tech Stack:** C++23, CppUTest, CMake/CTest, Python source extraction for the OLED probe, static HTML demo, DBT ARM toolchain for the release build.

**Spec:** `website/public/demos/loop-station.html` — `barsText`, `selectedTrackTitle`, and the `updateUI` OLED assignments are the visual and copy reference. `tests/loop_station/ui/selection_preview_test.py` pins the preview behaviour.

## Global Constraints

- OLED is 128×48. Title is centred at x=64, y=6 (`OLED_MAIN_TOPMOST_PIXEL + 1`), with `kTextTitleSpacingX` and `kTextTitleSizeY`.
- Rows sit at y=20 and y=30 with `kTextSpacingX` and `kTextSizeYUpdated`. There is no separator line.
- Exact copy: `LOOP STATION`; `TRACK {n}`; `NO TRACK`; `{bpm} BPM`; `IN: LEFT` / `IN: RIGHT` / `IN: STEREO` / `IN: BALANCED`; `READY`; `REC`; `{n} BAR` / `{n} BARS`.
- BPM is the nearest whole integer: `(tempoQ16 + 32768) / 65536`. 112.5 → `113 BPM`, 112.4999 → `112 BPM`, 1000 → `1000 BPM`.
- Bar count is the nearest whole bar, minimum 1: `max(1, (phraseFrames + framesPerBar / 2) / framesPerBar)`.
- No selection: row 2 shows `NO TRACK` with tempo still right-aligned; row 3 is absent. No model: only the centred title.
- Capture wins over empty and phrase states: a recording or overdub shows `REC`. A finished/stopped phrase shows its bar count. A slot with no phrase shows `READY`.
- Do not change selection gestures. Firmware keeps selecting on a border press; the demo's click-again-to-deselect is preview-only.
- Do not change `Engine`, `SongModel`, persistence, timing behaviour, memory-profile rows, or add dependencies.
- Keep the probe extraction contract: helpers go between `SongModel* model()` and `RGB colourFor(`; the method stays between `void LoopStationView::renderOLED(` and `void LoopStationView::selectEncoderAction(`.
- Do not start or restart the local HTTP server. The demo file also opens directly.

## Review Focus

- Tempo rounding boundaries: 112.5 must show 113, 112.4999 must show 112, and 1000 must stay 1000 without colliding with the track label.
- Capture/empty precedence: an initial recording has no phrase yet and must still show `REC`, not `READY`; an overdub with a phrase must also show `REC`.
- Bar estimate edges: a one-frame phrase must show `1 BAR` (never 0), a half-bar remainder must round up, and a cleared phrase must not retain a stale count.
- No model or invalid selection: no crash, no stale row-3 pixels, and tempo still shown whenever a model exists.
- Alignment and width: title centred at 64, tempo and bar labels right-aligned to x=127, and the longest pairs (`IN: BALANCED` + `1000 BPM`, `TRACK 8` + `1000 BPM`) must not overlap. Canvas canaries must stay untouched.

---

## Files and responsibilities

- `website/public/demos/loop-station.html` and `tests/loop_station/ui/selection_preview_test.py`: completed preview; committed by Task 1.
- `src/deluge/loop_station/timing.h` and `tests/loop_station/timing_tests.cpp`: read-only display accessors and their pinned values.
- `src/deluge/gui/views/loop_station_view.cpp`: the three-line renderer and its two private helpers.
- `tests/oled_canvas/probe_support.h`: probe declarations and clock/engine stubs.
- `tests/oled_canvas/generate_probe.py`: extraction of `drawStringCentred` and `drawStringAlignRight`.
- `tests/oled_canvas/loop_station_oled_tests.cpp`: selected, empty, capture, no-selection, no-model, fractional-tempo, and rounding image expectations.

## Execution environment

Run from the repository root. Prepend the bundled tools for CMake/CTest/DBT:

```bash
export PATH="$PWD/toolchain/current/python/bin:$PWD/toolchain/current/cmake/bin:$PWD/toolchain/current/clang-format:$PATH"
```

The existing host build is `build/tests`; the domain suite is `build/tests/loop_station/LoopStationTests`.

---

### Task 1: Commit the interactive preview

**Files:**
- Modify: `website/public/demos/loop-station.html`
- Modify: `tests/loop_station/ui/selection_preview_test.py`

**Interfaces:**
- Consumes: nothing.
- Produces: the committed design reference this plan implements. No firmware-facing API.

- [ ] **Step 1: Run both preview suites**

Run:

```bash
cd tests/loop_station/ui && python3 demo_contract_test.py && python3 selection_preview_test.py
```

Expected: `Ran 2 tests ... OK` and `Ran 4 tests ... OK`.

- [ ] **Step 2: Confirm only the preview files changed**

Run: `git status --short`

Expected: only `website/public/demos/loop-station.html` and `tests/loop_station/ui/selection_preview_test.py` are modified. Leave the untracked `.openchamber/` screenshots out of the commit.

- [ ] **Step 3: Commit**

```bash
git add website/public/demos/loop-station.html tests/loop_station/ui/selection_preview_test.py
git commit -m "docs: preview the three-line loop station screen"
```

---

### Task 2: Expose tempo and bar length for display

**Files:**
- Modify: `src/deluge/loop_station/timing.h`
- Test: `tests/loop_station/timing_tests.cpp`

**Interfaces:**
- Consumes: existing `ClockMap::samplesForBars(uint16_t)` and private `bpmQ16_`.
- Produces: `uint32_t ClockMap::tempoQ16() const` returning the current tempo in Q16, and `FrameIndex ClockMap::framesPerBar() const` returning `samplesForBars(1)`. Both are pure read-only accessors.

- [ ] **Step 1: Write the failing accessor test**

Add to `tests/loop_station/timing_tests.cpp`:

```cpp
TEST(LoopTiming, clockExposesTempoAndBarLengthForDisplay) {
	ClockMap clock;
	CHECK_EQUAL(120U * 65536U, clock.tempoQ16());
	CHECK_EQUAL(88200U, clock.framesPerBar());

	clock.setTempo(0, 60);
	CHECK_EQUAL(60U * 65536U, clock.tempoQ16());
	CHECK_EQUAL(176400U, clock.framesPerBar());

	clock.setTempoQ16(0, 112U * 65536U + 32768U);
	CHECK_EQUAL(112U * 65536U + 32768U, clock.tempoQ16());
	CHECK_EQUAL(94080U, clock.framesPerBar());

	clock.setTempoQ16(0, 1000U * 65536U);
	CHECK_EQUAL(10584U, clock.framesPerBar());

	clock.setTempoQ16(0, 0); // rejected; previous tempo remains
	CHECK_EQUAL(1000U * 65536U, clock.tempoQ16());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
cmake --build build/tests --target LoopStationTests -j4
```

Expected: compile failure naming `tempoQ16` and `framesPerBar` as missing members.

- [ ] **Step 3: Add the accessors**

In `src/deluge/loop_station/timing.h`, next to `samplesForBars`:

```cpp
uint32_t tempoQ16() const { return bpmQ16_; }
FrameIndex framesPerBar() const { return samplesForBars(1); }
```

`setTempoQ16` already rejects zero and values above 1000 BPM, so `framesPerBar()` is always at least 1.

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build build/tests --target LoopStationTests -j4
build/tests/loop_station/LoopStationTests -sg LoopTiming
```

Expected: all `LoopTiming` tests pass, including `clockExposesTempoAndBarLengthForDisplay`.

- [ ] **Step 5: Commit**

```bash
git add src/deluge/loop_station/timing.h tests/loop_station/timing_tests.cpp
git commit -m "feat: expose loop station tempo and bar length for display"
```

---

### Task 3: Render the three-line Loop Station screen

**Files:**
- Modify: `src/deluge/gui/views/loop_station_view.cpp:31-34` (helpers) and `:394-410` (`renderOLED`)
- Modify: `tests/oled_canvas/probe_support.h`
- Modify: `tests/oled_canvas/generate_probe.py`
- Test: `tests/oled_canvas/loop_station_oled_tests.cpp`

**Interfaces:**
- Consumes: `ClockMap::tempoQ16()` and `ClockMap::framesPerBar()` from Task 2; `SongModel::clock()`, `SongModel::engine()`, `Engine::selected()`, `Engine::settings()`, `Engine::state(SlotId)`; `Canvas::drawStringCentred(char const*, int32_t, int32_t, int32_t, int32_t = 64)` and `Canvas::drawStringAlignRight(char const*, int32_t, int32_t, int32_t, int32_t = 128)`.
- Produces: no public API. The rendered screen is the deliverable.

- [ ] **Step 1: Extend the probe fixture and generator**

In `tests/oled_canvas/probe_support.h`, include `"deluge/loop_station/engine.h"` and add the two canvas declarations:

```cpp
void drawStringCentred(char const*, int32_t, int32_t, int32_t, int32_t = OLED_MAIN_WIDTH_PIXELS / 2);
void drawStringAlignRight(char const*, int32_t, int32_t, int32_t, int32_t = OLED_MAIN_WIDTH_PIXELS);
```

Replace the `TestEngine` and `TestClock` bodies so the probe can supply the display state:

```cpp
struct TestEngine {
	Settings values = defaultSettings();
	SlotId selectedSlot = 0;
	std::array<SlotState, kTrackCount> slotStates{};
	Settings& settings() { return values; }
	const Settings& settings() const { return values; }
	SlotId selected() const { return selectedSlot; }
	SlotState state(SlotId slot) const { return validSlot(slot) ? slotStates[slot] : SlotState{}; }
};

struct TestClock {
	uint32_t bpmQ16 = 120 * 65536;
	FrameIndex framesPerBarValue = 88200;
	uint32_t tempoQ16() const { return bpmQ16; }
	FrameIndex framesPerBar() const { return framesPerBarValue; }
	uint32_t samplesForBars(uint32_t bars) const { return bars * 44100; }
};
```

In `tests/oled_canvas/generate_probe.py`, add to the `functions` list, after the `drawString` section:

```python
section(canvas, "void Canvas::drawStringCentred(", "void Canvas::drawStringCentered("),
```

and at the end of the list:

```python
section(canvas, "void Canvas::drawStringAlignRight(", "#define DO_CHARACTER_SCALING 0"),
```

Build the existing OLED tests to confirm the scaffolding compiles and the old tests still pass:

```bash
cmake --build build/tests --target LoopStationOledTests -j4
build/tests/oled_canvas/LoopStationOledTests
```

Expected: `LoopStationOledTests passed`.

- [ ] **Step 2: Replace the root-screen tests with the new expectations**

In `tests/oled_canvas/loop_station_oled_tests.cpp`, delete `composeRootExpected`, `testRootShowsAllStatusRows`, and `testRootWithoutModelShowsOnlyTitle`, and add:

```cpp
void composeMainExpected(Canvas& canvas, const char* trackLine, const char* tempoLine, const char* sourceLine,
                         const char* barsLine) {
	canvas.clear();
	constexpr int32_t titleY = OLED_MAIN_TOPMOST_PIXEL + 1;
	auto centredX = [&](const char* text, int32_t height) {
		return OLED_MAIN_WIDTH_PIXELS / 2 - (canvas.getStringWidthInPixels(text, height) >> 1);
	};
	auto rightX = [&](const char* text, int32_t height) {
		return OLED_MAIN_WIDTH_PIXELS - canvas.getStringWidthInPixels(text, height);
	};
	canvas.drawString("LOOP STATION", centredX("LOOP STATION", kTextTitleSizeY), titleY, kTextTitleSpacingX,
	                  kTextTitleSizeY);
	if (trackLine)
		canvas.drawString(trackLine, 0, 20, kTextSpacingX, kTextSizeYUpdated);
	if (tempoLine)
		canvas.drawString(tempoLine, rightX(tempoLine, kTextSizeYUpdated), 20, kTextSpacingX, kTextSizeYUpdated);
	if (sourceLine) {
		canvas.drawString(sourceLine, 0, 30, kTextSpacingX, kTextSizeYUpdated);
		canvas.drawString(barsLine, rightX(barsLine, kTextSizeYUpdated), 30, kTextSpacingX, kTextSizeYUpdated);
	}
}

void expectMainScreen(SongModel& model, const char* trackLine, const char* tempoLine, const char* sourceLine,
                      const char* barsLine) {
	Song song;
	song.model = &model;
	currentSong = &song;
	LoopStationView view;
	Canvas actual, expected;
	imageContext = trackLine;
	initialize(actual);
	composeMainExpected(expected, trackLine, tempoLine, sourceLine, barsLine);
	view.renderOLED(actual);
	compareImages(actual, expected);
	checkCanaries(actual);
}

void testEverySelectedTrackShowsItsNumberAndSource() {
	const char* sources[] = {"IN: LEFT", "IN: RIGHT", "IN: STEREO", "IN: BALANCED"};
	for (loop_station::SlotId slot = 0; slot < loop_station::kTrackCount; ++slot) {
		SongModel model;
		model.testEngine.selectedSlot = slot;
		model.testEngine.values.tracks[slot].source = static_cast<loop_station::Source>(slot % 4);
		char track[16];
		std::snprintf(track, sizeof(track), "TRACK %u", static_cast<unsigned>(slot) + 1);
		expectMainScreen(model, track, "120 BPM", sources[slot % 4], "READY");
	}
}

void testTempoRoundsToNearestWholeBpm() {
	SongModel model;
	model.testEngine.selectedSlot = 0;
	model.testClock.bpmQ16 = 112 * 65536 + 32768;
	expectMainScreen(model, "TRACK 1", "113 BPM", "IN: LEFT", "READY");
	model.testClock.bpmQ16 = 112 * 65536 + 32767;
	expectMainScreen(model, "TRACK 1", "112 BPM", "IN: LEFT", "READY");
	model.testClock.bpmQ16 = 1000 * 65536;
	expectMainScreen(model, "TRACK 1", "1000 BPM", "IN: LEFT", "READY");
}

void testEmptyCaptureAndPhraseLabels() {
	SongModel model;
	model.testEngine.selectedSlot = 2;
	model.testClock.framesPerBarValue = 1000;
	auto& state = model.testEngine.slotStates[2];

	expectMainScreen(model, "TRACK 3", "120 BPM", "IN: LEFT", "READY");
	state.capture = true;
	expectMainScreen(model, "TRACK 3", "120 BPM", "IN: LEFT", "REC");
	state.hasPhrase = true;
	state.playing = true; // overdub still reports capture
	expectMainScreen(model, "TRACK 3", "120 BPM", "IN: LEFT", "REC");
	state.capture = false;
	state.playing = false;
	state.phraseFrames = 4000;
	expectMainScreen(model, "TRACK 3", "120 BPM", "IN: LEFT", "4 BARS");
	state.hasPhrase = false;
	state.phraseFrames = 0; // cleared phrase must not retain the old count
	expectMainScreen(model, "TRACK 3", "120 BPM", "IN: LEFT", "READY");
}

void testBarCountRoundsToNearestAndNeverShowsZero() {
	SongModel model;
	model.testEngine.selectedSlot = 0;
	model.testClock.framesPerBarValue = 1000;
	auto& state = model.testEngine.slotStates[0];
	state.hasPhrase = true;
	const struct {
		loop_station::FrameIndex frames;
		const char* label;
	} cases[] = {{1, "1 BAR"}, {1499, "1 BAR"}, {1500, "2 BARS"}, {64000, "64 BARS"}};
	for (const auto& c : cases) {
		state.phraseFrames = c.frames;
		expectMainScreen(model, "TRACK 1", "120 BPM", "IN: LEFT", c.label);
	}
}

void testNoSelectionShowsNoTrackAndNoInfoRow() {
	SongModel model;
	model.testEngine.selectedSlot = loop_station::kInvalidSlot;
	expectMainScreen(model, "NO TRACK", "120 BPM", nullptr, nullptr);
}

void testNoModelShowsOnlyCentredTitle() {
	currentSong = nullptr;
	LoopStationView view;
	Canvas actual, expected;
	imageContext = "no model";
	initialize(actual);
	composeMainExpected(expected, nullptr, nullptr, nullptr, nullptr);
	view.renderOLED(actual);
	compareImages(actual, expected);
	checkCanaries(actual);
}

void testLongestLinesFitWithoutOverlap() {
	Canvas canvas;
	auto width = [&](const char* text) { return canvas.getStringWidthInPixels(text, kTextSizeYUpdated); };
	CHECK(width("IN: BALANCED") + width("1000 BPM") < OLED_MAIN_WIDTH_PIXELS);
	CHECK(width("TRACK 8") + width("1000 BPM") < OLED_MAIN_WIDTH_PIXELS);
	CHECK(width("NO TRACK") + width("1000 BPM") < OLED_MAIN_WIDTH_PIXELS);
	CHECK(width("IN: STEREO") + width("64 BARS") < OLED_MAIN_WIDTH_PIXELS);
}
```

Update `main()` to call the seven new tests in place of the two removed root tests. Keep `testSettingsSelectionStaysVisible` and `testCopyTargetAndConfirmationFit` unchanged.

- [ ] **Step 3: Run the new tests to verify they fail**

```bash
cmake --build build/tests --target LoopStationOledTests -j4
build/tests/oled_canvas/LoopStationOledTests
```

Expected: build succeeds, then image mismatches for the main-screen cases (old `Loop Track n` / memory rows remain). Record the first mismatch line as the RED evidence.

- [ ] **Step 4: Add the helpers and rewrite `renderOLED`**

In `src/deluge/gui/views/loop_station_view.cpp`, insert these helpers in the anonymous namespace immediately after `model()` and before `colourFor(`:

```cpp
char const* sourceLabel(Source source) {
	switch (source) {
	case Source::Left:
		return "LEFT";
	case Source::Right:
		return "RIGHT";
	case Source::Stereo:
		return "STEREO";
	case Source::Balanced:
		return "BALANCED";
	}
	return "LEFT";
}

void formatBarCount(const SlotState& state, FrameIndex framesPerBar, char* out, size_t size) {
	if (state.capture)
		snprintf(out, size, "REC");
	else if (!state.hasPhrase)
		snprintf(out, size, "READY");
	else {
		uint32_t bars = static_cast<uint32_t>((uint64_t(state.phraseFrames) + framesPerBar / 2) / framesPerBar);
		if (bars < 1)
			bars = 1;
		snprintf(out, size, "%u BAR%s", bars, bars == 1 ? "" : "S");
	}
}
```

Replace `renderOLED` with:

```cpp
void LoopStationView::renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	canvas.clear();
	canvas.drawStringCentred("LOOP STATION", OLED_MAIN_TOPMOST_PIXEL + 1, kTextTitleSpacingX, kTextTitleSizeY);
	auto* m = model();
	if (!m)
		return;

	const SlotId selected = m->engine().selected();
	const bool hasTrack = validSlot(selected);
	char line[24];
	if (hasTrack)
		snprintf(line, sizeof(line), "TRACK %u", static_cast<unsigned>(selected) + 1);
	else
		snprintf(line, sizeof(line), "NO TRACK");
	canvas.drawString(line, 0, 20, kTextSpacingX, kTextSizeYUpdated);

	const uint32_t bpm = (m->clock().tempoQ16() + 32768) / 65536;
	snprintf(line, sizeof(line), "%u BPM", bpm);
	canvas.drawStringAlignRight(line, 20, kTextSpacingX, kTextSizeYUpdated);

	if (!hasTrack)
		return;

	snprintf(line, sizeof(line), "IN: %s", sourceLabel(m->engine().settings().tracks[selected].source));
	canvas.drawString(line, 0, 30, kTextSpacingX, kTextSizeYUpdated);

	formatBarCount(m->engine().state(selected), m->clock().framesPerBar(), line, sizeof(line));
	canvas.drawStringAlignRight(line, 30, kTextSpacingX, kTextSizeYUpdated);
}
```

`capture` is checked before `hasPhrase` so an initial recording still shows `REC`; `framesPerBar()` is guaranteed non-zero by the validated clock.

- [ ] **Step 5: Run the OLED tests to verify they pass**

```bash
cmake --build build/tests --target LoopStationOledTests -j4
build/tests/oled_canvas/LoopStationOledTests
```

Expected: `LoopStationOledTests passed`. If an image mismatch names a column, compare the failing row against the expected string positions before changing tests.

- [ ] **Step 6: Run the full host verification**

```bash
cmake --build build/tests --target LoopStationTests OledCanvasTests LoopStationOledTests -j4
ctest --test-dir build/tests -R '^(LoopStationTests|OledCanvasTests|LoopStationOledTests)$' --output-on-failure
python3 tests/loop_station/ui/demo_contract_test.py
python3 tests/loop_station/ui/selection_preview_test.py
```

Expected: all three CTest suites pass, plus both Python preview suites.

- [ ] **Step 7: Build the ARM release**

```bash
DBT_NO_SYNC=1 ./dbt build Release
```

Expected: successful ARM compile and link.

- [ ] **Step 8: Check and commit**

Run `git diff --check` and review the diff for accidental changes to selection or engine code. Then:

```bash
git add src/deluge/gui/views/loop_station_view.cpp tests/oled_canvas/probe_support.h \
        tests/oled_canvas/generate_probe.py tests/oled_canvas/loop_station_oled_tests.cpp
git commit -m "feat: show track, input and bars on the loop station screen"
```

---

## Plan self-review

Spec coverage: Task 1 pins the preview. Task 2 supplies the tempo and bar-length values the screen needs. Task 3 covers every visible line, all four input labels, all eight track numbers, both status labels, both alignment rules, and both no-selection/no-model states.

Review Focus tests: tempo boundaries and width fit are in `testTempoRoundsToNearestWholeBpm` and `testLongestLinesFitWithoutOverlap`; capture/empty precedence and overdub are in `testEmptyCaptureAndPhraseLabels`; bar rounding and the one-frame minimum are in `testBarCountRoundsToNearestAndNeverShowsZero`; no selection/no model are separate image tests; centring and right alignment are pinned by the expected composer's manual x positions, with canaries checked on every render.

Type consistency: Task 2 produces `tempoQ16()`/`framesPerBar()` exactly as the probe and renderer consume them. The probe's `TestClock` and `TestEngine` return types match the real `ClockMap` and `Engine` signatures used by `renderOLED`.
