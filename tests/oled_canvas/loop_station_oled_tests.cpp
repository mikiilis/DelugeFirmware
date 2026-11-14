#include "probe_support.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using deluge::gui::menu_item::loop_station::SettingsMenu;
using loop_station::MemoryProfile;
using loop_station::MenuField;
using loop_station::MenuScope;
using loop_station::Settings;
using loop_station::SongModel;

namespace loop_station {
MemoryProfile testConfiguredMemoryProfile = MemoryProfile::Preferred;
}

Song* currentSong = nullptr;

namespace deluge::l10n {
const char* get(String string) {
	switch (string) {
	case String::STRING_FOR_LOOP_STATION_GLOBAL_SETTINGS:
		return "Global Settings";
	case String::STRING_FOR_LOOP_STATION_TRACK_SETTINGS:
		return "Track Settings";
	case String::STRING_FOR_LOOP_STATION_RECORD_ACTION:
		return "Record Action";
	case String::STRING_FOR_LOOP_STATION_START_MODE:
		return "Start Mode";
	case String::STRING_FOR_LOOP_STATION_START_INTERVAL:
		return "Start Interval";
	case String::STRING_FOR_LOOP_STATION_STOP_MODE:
		return "Stop Mode";
	case String::STRING_FOR_LOOP_STATION_STOP_INTERVAL:
		return "Stop Interval";
	case String::STRING_FOR_LOOP_STATION_INPUT_SOURCE:
		return "Input Source";
	case String::STRING_FOR_LOOP_STATION_MONITORING:
		return "Monitoring";
	case String::STRING_FOR_LOOP_STATION_DURATION:
		return "Duration";
	case String::STRING_FOR_LOOP_STATION_PLAYBACK:
		return "Playback";
	case String::STRING_FOR_LOOP_STATION_TRANSPOSE:
		return "Transpose";
	case String::STRING_FOR_LOOP_STATION_TRACK_COLOUR:
		return "Track Colour";
	case String::STRING_FOR_LOOP_STATION_COPY_TO_TRACK:
		return "Copy To Track";
	case String::STRING_FOR_LOOP_STATION_COPY_FROM:
		return "From Track %u";
	case String::STRING_FOR_LOOP_STATION_COPY_TARGET:
		return "To Track %u";
	case String::STRING_FOR_LOOP_STATION_COPY_CONFIRM:
		return "SELECT: copy BACK: cancel";
	case String::STRING_FOR_LOOP_STATION_COPY_REVIEW:
		return "SELECT to review";
	}
	return "Setting";
}
} // namespace deluge::l10n

namespace {
int failures = 0;
const char* imageContext = "unspecified";

void check(bool condition, char const* expression, int line) {
	if (!condition) {
		std::fprintf(stderr, "line %d: %s\n", line, expression);
		++failures;
	}
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void initialize(Canvas& canvas) {
	canvas.clear();
	canvas.leadingCanary.fill(0xa5);
	canvas.trailingCanary.fill(0xa5);
}

void checkCanaries(const Canvas& canvas) {
	for (auto byte : canvas.leadingCanary)
		CHECK(byte == 0xa5);
	for (auto byte : canvas.trailingCanary)
		CHECK(byte == 0xa5);
}

void compareImages(const Canvas& actual, const Canvas& expected) {
	int different = 0;
	int firstRow = -1;
	int firstColumn = -1;
	for (size_t row = 0; row < Canvas::kImageHeight; ++row) {
		for (size_t column = 0; column < Canvas::kImageWidth; ++column) {
			if (actual.image_[row][column] != expected.image_[row][column]) {
				if (firstRow < 0) {
					firstRow = row;
					firstColumn = column;
				}
				++different;
			}
		}
	}
	if (different) {
		std::fprintf(stderr, "%s image mismatch: %d bytes; first at (%d,%d), actual=%02x expected=%02x\n", imageContext,
		             different, firstColumn, firstRow, actual.image_[firstRow][firstColumn],
		             expected.image_[firstRow][firstColumn]);
		++failures;
	}
}

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

struct ExpectedRow {
	const char* label;
	const char* value;
};

void composeSettingsExpected(Canvas& canvas, const char* title, const std::vector<ExpectedRow>& rows, uint8_t selected,
                             bool editing = false) {
	canvas.clear();
	canvas.drawString(title, 0, 6, kTextTitleSpacingX, kTextTitleSizeY);
	const size_t first = std::min<size_t>(selected > 0 ? selected - 1 : 0, rows.size() > 3 ? rows.size() - 3 : 0);
	const size_t end = std::min(first + 3, rows.size());
	for (size_t i = first; i < end; ++i) {
		char line[64];
		std::snprintf(line, sizeof(line), "%c%s: %s", i == selected ? (editing ? '*' : '>') : ' ', rows[i].label,
		              rows[i].value);
		canvas.drawString(line, 0, 20 + static_cast<int32_t>(i - first) * kTextSpacingY, kTextSpacingX,
		                  kTextSizeYUpdated);
	}
}

std::vector<ExpectedRow> globalRows(const Settings& settings) {
	std::vector<ExpectedRow> rows{
	    {"Record Action", "REC PLAY DUB"},
	    {"Start Mode", settings.startAction.mode == loop_station::ActionMode::Synced ? "Synced" : "Immediate"}};
	if (settings.startAction.mode == loop_station::ActionMode::Synced)
		rows.push_back({"Start Interval", "1/16"});
	rows.push_back(
	    {"Stop Mode", settings.stopAction.mode == loop_station::ActionMode::Synced ? "Synced" : "Immediate"});
	if (settings.stopAction.mode == loop_station::ActionMode::Synced)
		rows.push_back({"Stop Interval", "1/16"});
	return rows;
}

void testSettingsSelectionStaysVisible() {
	SongModel model;
	Song song;
	song.model = &model;
	currentSong = &song;
	SettingsMenu menu;
	for (bool startSynced : {false, true}) {
		for (bool stopSynced : {false, true}) {
			model.testEngine.values.startAction.mode =
			    startSynced ? loop_station::ActionMode::Synced : loop_station::ActionMode::Immediate;
			model.testEngine.values.stopAction.mode =
			    stopSynced ? loop_station::ActionMode::Synced : loop_station::ActionMode::Immediate;
			menu.scope_ = MenuScope::Global;
			const auto rows = globalRows(model.testEngine.values);
			for (uint8_t selected = 0; selected < rows.size(); ++selected) {
				menu.row_ = selected;
				Canvas actual, expected;
				imageContext = "global settings";
				initialize(actual);
				composeSettingsExpected(expected, "Global Settings", rows, selected);
				menu.renderOLED(actual);
				compareImages(actual, expected);
				checkCanaries(actual);
			}
		}
	}

	menu.scope_ = MenuScope::Track;
	const std::vector<ExpectedRow> rows{
	    {"Input Source", "Left"},  {"Monitoring", "On"},       {"Duration", "MANUAL"},      {"Playback", "Loop"},
	    {"Transpose", "+0.00 st"}, {"Track Colour", "Yellow"}, {"Copy To Track", "Track 2"}};
	for (uint8_t selected = 0; selected < rows.size(); ++selected) {
		menu.row_ = selected;
		for (bool editing : {false, true}) {
			menu.editing_ = editing;
			Canvas actual, expected;
			imageContext = "track settings";
			initialize(actual);
			composeSettingsExpected(expected, "Track Settings", rows, selected, editing);
			menu.renderOLED(actual);
			compareImages(actual, expected);
			checkCanaries(actual);
		}
	}
}

void testCopyTargetAndConfirmationFit() {
	SongModel model;
	Song song;
	song.model = &model;
	currentSong = &song;
	SettingsMenu menu;
	menu.scope_ = MenuScope::Track;
	menu.choosingCopyTarget_ = true;
	for (loop_station::SlotId slot : {loop_station::SlotId{0}, loop_station::SlotId{7}}) {
		for (bool review : {false, true}) {
			menu.copyReview_ = review;
			menu.copyReviewTarget_ = slot;
			menu.copyTarget_ = review ? (slot + 1) % 8 : slot; // review must show its frozen target
			Canvas actual, expected;
			imageContext = "copy target";
			initialize(actual);
			expected.clear();
			expected.drawString("Track Settings", 0, 6, kTextTitleSpacingX, kTextTitleSizeY);
			char target[24];
			expected.drawString("From Track 1", 0, 19, kTextSpacingX, kTextSizeYUpdated);
			std::snprintf(target, sizeof(target), "To Track %u", slot + 1);
			expected.drawString(target, 0, 28, kTextSpacingX, kTextSizeYUpdated);
			expected.drawString(review ? "SELECT: copy BACK: cancel" : "SELECT to review", 0, 39, kTextSpacingX,
			                    kTextSmallSizeY);
			menu.renderOLED(actual);
			compareImages(actual, expected);
			checkCanaries(actual);
		}
	}
}
} // namespace

int main() {
	testEverySelectedTrackShowsItsNumberAndSource();
	testTempoRoundsToNearestWholeBpm();
	testEmptyCaptureAndPhraseLabels();
	testBarCountRoundsToNearestAndNeverShowsZero();
	testNoSelectionShowsNoTrackAndNoInfoRow();
	testNoModelShowsOnlyCentredTitle();
	testLongestLinesFitWithoutOverlap();
	testSettingsSelectionStaysVisible();
	testCopyTargetAndConfirmationFit();
	if (failures) {
		std::fprintf(stderr, "%d Loop Station OLED checks failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("LoopStationOledTests passed");
	return EXIT_SUCCESS;
}
