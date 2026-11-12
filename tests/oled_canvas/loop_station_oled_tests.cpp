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

void composeRootExpected(Canvas& canvas, MemoryProfile profile, bool unsaved, bool undo) {
	canvas.clear();
	canvas.drawString("Loop Station", 0, 6, kTextTitleSpacingX, kTextTitleSizeY);
	canvas.drawString(profile == MemoryProfile::TenSecond ? "26.9 MiB / 10s" : "32 MiB / ~12s x8", 0, 20, kTextSpacingX,
	                  kTextSizeYUpdated);
	canvas.drawString(unsaved ? "Unsaved audio" : "Saved", 0, 30, kTextSpacingX, kTextSizeYUpdated);
	canvas.drawString(undo ? "Undo ready" : "Undo unavailable", 0, 40, kTextSpacingX, kTextSizeYUpdated);
}

void testRootShowsAllStatusRows() {
	SongModel model;
	Song song;
	song.model = &model;
	currentSong = &song;
	LoopStationView view;
	for (auto profile : {MemoryProfile::Preferred, MemoryProfile::TenSecond}) {
		loop_station::testConfiguredMemoryProfile = profile;
		for (bool unsaved : {false, true}) {
			for (bool undo : {false, true}) {
				model.unsaved = unsaved;
				model.undoAvailable = undo;
				Canvas actual, expected;
				imageContext = "root rows";
				initialize(actual);
				composeRootExpected(expected, profile, unsaved, undo);
				view.renderOLED(actual);
				compareImages(actual, expected);
				checkCanaries(actual);
			}
		}
	}
}

void testRootWithoutModelShowsOnlyTitle() {
	currentSong = nullptr;
	LoopStationView view;
	Canvas actual, expected;
	imageContext = "root without model";
	initialize(actual);
	expected.clear();
	expected.drawString("Loop Station", 0, 6, kTextTitleSpacingX, kTextTitleSizeY);
	view.renderOLED(actual);
	compareImages(actual, expected);
	checkCanaries(actual);
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
	for (bool synced : {false, true}) {
		model.testEngine.values.startAction.mode =
		    synced ? loop_station::ActionMode::Synced : loop_station::ActionMode::Immediate;
		model.testEngine.values.stopAction.mode =
		    synced ? loop_station::ActionMode::Synced : loop_station::ActionMode::Immediate;
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
		menu.copyTarget_ = slot;
		Canvas actual, expected;
		imageContext = "copy target";
		initialize(actual);
		expected.clear();
		expected.drawString("Track Settings", 0, 6, kTextTitleSpacingX, kTextTitleSizeY);
		char target[24];
		std::snprintf(target, sizeof(target), "Copy to Track %u", slot + 1);
		expected.drawString(target, 0, 22, kTextSpacingX, kTextSizeYUpdated);
		expected.drawString("SELECT to copy", 0, 38, kTextSpacingX, kTextSizeYUpdated);
		menu.renderOLED(actual);
		compareImages(actual, expected);
		checkCanaries(actual);
	}
}
} // namespace

int main() {
	testRootShowsAllStatusRows();
	testRootWithoutModelShowsOnlyTitle();
	testSettingsSelectionStaysVisible();
	testCopyTargetAndConfirmationFit();
	if (failures) {
		std::fprintf(stderr, "%d Loop Station OLED checks failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("LoopStationOledTests passed");
	return EXIT_SUCCESS;
}
