#pragma once

#include "definitions_cxx.hpp"
#include "deluge/loop_station/geometry.h"
#include "deluge/loop_station/menu_model.h"
#include "deluge/loop_station/song_model.h"
#include "deluge/loop_station/view_state.h"
#include "gui/colour/palette.h"
#include "hid/button.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Only firmware boundaries are replaced: handlers, menu logic, model and engine are real.
class ProbeUI {
public:
	virtual ~ProbeUI() = default;
	virtual bool opened() { return true; }
	void close();
};
extern ProbeUI* probeCurrentUI;
extern ProbeUI* probeRootUI;
extern unsigned probeRenderRequests;
extern std::vector<ProbeUI*> probeRenderTargets;
extern unsigned probeOledRequests;
extern uint64_t probeSampleClock;
namespace AudioEngine {
uint64_t loopStationSampleClock();
}
bool openUI(ProbeUI* ui);
void changeRootUI(ProbeUI* ui);
ProbeUI* getCurrentUI();
ProbeUI* getRootUI();
void uiNeedsRendering(ProbeUI* ui);
void renderUIsForOled();

class Song {
public:
	loop_station::SongModel* model = nullptr;
	loop_station::SongModel* loopStation() { return model; }
	loop_station::Status ensureLoopStation(bool) {
		if (!model)
			return loop_station::Status::Unavailable;
		return model->ready() ? loop_station::Status::Ok : model->start();
	}
};
extern Song* currentSong;
extern bool sdRoutineLock;
struct ProbeDisplay {
	bool oled = true;
	std::string text;
	std::vector<std::string> popups;
	bool haveOLED() const { return oled; }
	bool have7SEG() const { return !oled; }
	bool notification = false;
	bool hasPopupOfType(auto) const { return notification; }
	void cancelPopup() { notification = false; }
	void displayNotification(const char*, const char*) { notification = true; }
	void setText(const char* value) { text = value; }
	void displayPopup(const char* value) { popups.emplace_back(value); }
};
extern ProbeDisplay* display;
namespace Buttons {
extern bool shiftPressed;
inline bool selectPressed = false, selectButtonPressUsedUp = false;
inline bool isButtonPressed(deluge::hid::Button b) {
	return (b == deluge::hid::button::SHIFT && shiftPressed) || (b == deluge::hid::button::SELECT_ENC && selectPressed);
}
inline bool isAnyOfButtonsPressed(std::initializer_list<deluge::hid::Button> buttons) {
	return std::ranges::any_of(buttons, isButtonPressed);
}
inline bool isShiftButtonPressed() {
	return shiftPressed;
}
} // namespace Buttons
enum class IndicatorLED { CROSS_SCREEN_EDIT, SESSION_VIEW, SCALE_MODE, SYNTH, KIT, MIDI, CV };
namespace indicator_leds {
inline std::map<IndicatorLED, bool> states;
inline void setLedState(IndicatorLED led, bool on) {
	states[led] = on;
}
inline void blinkLed(IndicatorLED) {
}
} // namespace indicator_leds
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
	STRING_FOR_LOOP_STATION_TRACK_UNAVAILABLE,
	STRING_FOR_LOOP_STATION_COPIED,
	STRING_FOR_LOOP_STATION_COPY_BUSY,
};
const char* get(String);
} // namespace deluge::l10n
struct ProbeSessionView : ProbeUI {
	AudioClip* viewedClip = nullptr;
	void transitionToViewForClip(AudioClip* clip) { viewedClip = clip; }
};
extern ProbeSessionView sessionView;

namespace deluge::hid::display::oled_canvas {
class Canvas {
public:
	std::vector<std::string> lines;
	void clear() { lines.clear(); }
	void drawString(std::string_view text, int32_t, int32_t, int32_t, int32_t) { lines.emplace_back(text); }
	int inversions = 0, underlines = 0, pixels = 0;
	int getStringWidthInPixels(const char* text, int) { return std::string_view(text).size() * 6; }
	void drawScreenTitle(std::string_view text, bool) { lines.emplace_back(text); }
	void drawLine(int, int, int, int) {}
	void drawStringCentered(const char* text, int, int, int, int, int) { lines.emplace_back(text); }
	void invertAreaRounded(int, int, int, int) { ++inversions; }
	void drawHorizontalLine(int, int, int) { ++underlines; }
	void drawPixel(int, int) { ++pixels; }
};
} // namespace deluge::hid::display::oled_canvas

// Probe declarations avoid firmware RootUI dependencies. Firmware compilation separately
// checks production declarations; all method bodies are extracted verbatim.
class LoopStationView : public ProbeUI {
public:
	bool opened() override;
	void focusRegained();
	void graphicsRoutine();
	ActionResult timerCallback();
	ActionResult padAction(int32_t, int32_t, int32_t);
	ActionResult buttonAction(deluge::hid::Button, bool, bool);
	bool renderMainPads(uint32_t, RGB[][kDisplayWidth + kSideBarWidth], uint8_t[][kDisplayWidth + kSideBarWidth], bool);
	bool renderSidebar(uint32_t, RGB[][kDisplayWidth + kSideBarWidth], uint8_t[][kDisplayWidth + kSideBarWidth]);
	void selectEncoderAction(int8_t);
	bool prepareTrack(loop_station::SlotId);
	void updateLed();
	void attemptExit();
	loop_station::RestoreId pendingRestore_{};
	loop_station::SlotId pendingRestoreSlot_ = loop_station::kInvalidSlot;
	bool pendingExit_ = false;
};
namespace deluge::gui::menu_item::loop_station {
class SettingsMenu : public ProbeUI {
public:
	bool openGlobal();
	bool openTrack(::loop_station::SlotId);
	bool opened() override;
	ActionResult buttonAction(deluge::hid::Button, bool, bool);
	ActionResult padAction(int32_t, int32_t, int32_t);
	ActionResult horizontalEncoderAction(int32_t);
	void selectEncoderAction(int8_t);
	void renderOLED(deluge::hid::display::oled_canvas::Canvas&);
	bool open(::loop_station::MenuScope, ::loop_station::SlotId);
	void changeValue(int32_t);
	void selectCurrent();
	void closeMenu();
	void refreshDisplay();
	::loop_station::MenuFields fields_{};
	::loop_station::MenuScope scope_ = ::loop_station::MenuScope::Global;
	::loop_station::SlotId slot_ = 0;
	::loop_station::SlotId copyTarget_ = 1;
	uint8_t row_ = 0;
	bool editing_ = false;
	bool choosingCopyTarget_ = false;
};
extern SettingsMenu loopStationSettingsMenu;
} // namespace deluge::gui::menu_item::loop_station
