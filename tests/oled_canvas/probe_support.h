#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "deluge/loop_station/config.h"
#include "deluge/loop_station/menu_model.h"
#include "deluge/loop_station/types.h"
#include "gui/fonts/fonts.h"

constexpr int32_t OLED_MAIN_WIDTH_PIXELS = 128;
constexpr int32_t OLED_MAIN_HEIGHT_PIXELS = 48;
constexpr int32_t OLED_MAIN_TOPMOST_PIXEL = 5;
constexpr int32_t kTextSpacingX = 6;
constexpr int32_t kTextSpacingY = 9;
constexpr int32_t kTextSizeYUpdated = 7;
constexpr int32_t kTextTitleSpacingX = 9;
constexpr int32_t kTextTitleSizeY = 10;
constexpr int32_t kTextSmallSizeY = 5;

namespace deluge::hid::display::oled_canvas {
class Canvas {
public:
	static constexpr int32_t kImageHeight = OLED_MAIN_HEIGHT_PIXELS >> 3;
	static constexpr int32_t kImageWidth = OLED_MAIN_WIDTH_PIXELS;
	using ImageStore = uint8_t[kImageHeight][kImageWidth];

	std::array<uint8_t, 128> leadingCanary{};
	ImageStore image_{};
	std::array<uint8_t, 128> trailingCanary{};

	void clear() { std::fill(&image_[0][0], &image_[0][0] + kImageHeight * kImageWidth, 0); }
	ImageStore& hackGetImageStore() { return image_; }

	void drawString(std::string_view, int32_t, int32_t, int32_t, int32_t, int32_t = 0, int32_t = 128, bool = false);
	void drawChar(uint8_t, int32_t, int32_t, int32_t, int32_t, int32_t = 0, int32_t = 128);
	int32_t getCharIndex(uint8_t);
	int32_t getCharWidthInPixels(uint8_t, int32_t);
	int32_t getCharSpacingInPixels(uint8_t, int32_t, bool);
	int32_t getStringWidthInPixels(char const*, int32_t);
	void drawGraphicMultiLine(uint8_t const*, int32_t, int32_t, int32_t, int32_t, int32_t, bool = false);
};
} // namespace deluge::hid::display::oled_canvas

using deluge::hid::display::oled_canvas::Canvas;

namespace loop_station {
extern MemoryProfile testConfiguredMemoryProfile;

struct TestEngine {
	Settings values = defaultSettings();
	SlotId selectedSlot = 0;
	Settings& settings() { return values; }
	const Settings& settings() const { return values; }
	SlotId selected() const { return selectedSlot; }
};

struct TestClock {
	uint32_t samplesForBars(uint32_t bars) const { return bars * 44100; }
};

class SongModel {
public:
	bool unsaved = false;
	bool undoAvailable = false;
	TestEngine testEngine{};
	TestClock testClock{};
	TestEngine& engine() { return testEngine; }
	const TestEngine& engine() const { return testEngine; }
	bool hasUnsavedAudio() const { return unsaved; }
	bool canUndo(SlotId) const { return undoAvailable; }
	uint16_t maxDurationBarsForTrack(SlotId, uint32_t) const { return 64; }
	const TestClock& clock() const { return testClock; }
};
} // namespace loop_station

class Song {
public:
	loop_station::SongModel* loopStation() { return model; }
	loop_station::SongModel* model = nullptr;
};
extern Song* currentSong;

namespace deluge::l10n {
enum class String {
	STRING_FOR_LOOP_STATION_GLOBAL_SETTINGS,
	STRING_FOR_LOOP_STATION_TRACK_SETTINGS,
	STRING_FOR_LOOP_STATION_RECORD_ACTION,
	STRING_FOR_LOOP_STATION_START_MODE,
	STRING_FOR_LOOP_STATION_START_INTERVAL,
	STRING_FOR_LOOP_STATION_STOP_MODE,
	STRING_FOR_LOOP_STATION_STOP_INTERVAL,
	STRING_FOR_LOOP_STATION_INPUT_SOURCE,
	STRING_FOR_LOOP_STATION_MONITORING,
	STRING_FOR_LOOP_STATION_DURATION,
	STRING_FOR_LOOP_STATION_PLAYBACK,
	STRING_FOR_LOOP_STATION_TRANSPOSE,
	STRING_FOR_LOOP_STATION_TRACK_COLOUR,
	STRING_FOR_LOOP_STATION_COPY_TO_TRACK,
	STRING_FOR_LOOP_STATION_COPY_FROM,
	STRING_FOR_LOOP_STATION_COPY_TARGET,
	STRING_FOR_LOOP_STATION_COPY_CONFIRM,
	STRING_FOR_LOOP_STATION_COPY_REVIEW
};
const char* get(String string);
} // namespace deluge::l10n

namespace deluge::gui::menu_item::loop_station {
class SettingsMenu {
public:
	// This existing real-canvas suite exercises the preference-off list path.
	// Integrated horizontal rendering is covered separately, not replaced with a fake menu.
	void renderOLED(Canvas& canvas) {
		canvas.clear();
		renderListOLED(canvas);
	}
	void renderListOLED(Canvas& canvas);
	bool copyReview_ = false;
	::loop_station::SlotId copyReviewTarget_ = 1;
	::loop_station::MenuFields fields_{};
	::loop_station::MenuScope scope_ = ::loop_station::MenuScope::Global;
	::loop_station::SlotId slot_ = 0;
	::loop_station::SlotId copyTarget_ = 1;
	uint8_t row_ = 0;
	bool editing_ = false;
	bool choosingCopyTarget_ = false;
};
} // namespace deluge::gui::menu_item::loop_station

class LoopStationView {
public:
	void renderOLED(Canvas& canvas);
};
