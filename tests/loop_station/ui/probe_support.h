#pragma once

#include "definitions_cxx.hpp"
#include "deluge/loop_station/geometry.h"
#include "deluge/loop_station/length_display.h"
#include "deluge/loop_station/menu_model.h"
#include "deluge/loop_station/song_model.h"
#include "deluge/loop_station/view_state.h"
#include "gui/colour/palette.h"
#include "hid/button.h"
#include "strings_declarations.h"
#include <algorithm>
#include <array>
#include <cstddef>
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
	virtual void focusRegained() {}
	virtual void graphicsRoutine();
	virtual bool renderMainPads(uint32_t = 0, RGB image[][kDisplayWidth + kSideBarWidth] = nullptr,
	                            uint8_t mask[][kDisplayWidth + kSideBarWidth] = nullptr, bool = true) {
		return false;
	}
	virtual bool renderSidebar(uint32_t = 0, RGB image[][kDisplayWidth + kSideBarWidth] = nullptr,
	                           uint8_t mask[][kDisplayWidth + kSideBarWidth] = nullptr) {
		return false;
	}
	virtual bool getAffectEntire() { return false; }
	virtual UIModControllableContext getUIModControllableContext() { return UIModControllableContext::NONE; }
	void close();
};
struct ProbePadFrame {
	RGB focused;
	RGB other;
};
extern std::vector<ProbePadFrame> probeSentPadFrames;
void probeDispatchGraphicsTick(bool uartReady);
extern ProbeUI* probeCurrentUI;
extern ProbeUI* probeRootUI;
extern unsigned probeRenderRequests;
extern std::vector<ProbeUI*> probeRenderTargets;
extern std::vector<std::pair<uint32_t, uint32_t>> probeRenderRows;
extern unsigned probeOledRequests;
extern uint64_t probeSampleClock;
namespace AudioEngine {
uint64_t loopStationSampleClock();
}
bool openUI(ProbeUI* ui);
void changeRootUI(ProbeUI* ui);
ProbeUI* getCurrentUI();
ProbeUI* getRootUI();
void uiNeedsRendering(ProbeUI* ui, uint32_t mainRows = 0xffffffff, uint32_t sideRows = 0xffffffff);
void renderUIsForOled();

class TimelineCounter {
public:
	virtual ~TimelineCounter() = default;
};

class Song : public TimelineCounter {
public:
	loop_station::SongModel* model = nullptr;
	void* currentClip = nullptr;
	bool affectEntire = false;
	void setCurrentClip(void* clip) { currentClip = clip; }
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
	std::string notificationName, notificationValue;
	bool hasPopupOfType(auto) const { return notification; }
	void cancelPopup() { notification = false; }
	void displayNotification(const char* name, const char* value) {
		notification = true;
		notificationName = name;
		notificationValue = value;
	}
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
inline std::map<String, std::string> probeTranslations;
const char* get(String);
} // namespace deluge::l10n
struct ProbeSessionView : ProbeUI {
	AudioClip* viewedClip = nullptr;
	void transitionToViewForClip(AudioClip* clip) { viewedClip = clip; }
};
extern ProbeSessionView sessionView;

// Shared View boundary. The spy records exact forwarded arguments and returns a
// configurable result; it deliberately implements no transport or metronome behavior.
#define UI_MODE_NONE 0
#define UI_MODE_MIDI_LEARN 11
#define UI_MODE_RECORD_COUNT_IN 45
#define UI_MODE_HOLDING_SAVE_BUTTON 51
#define UI_MODE_HOLDING_LOAD_BUTTON 52
#define EXCLUSIVE_UI_MODES_MASK ((uint32_t)255)
#define UI_MODE_STUTTERING ((uint32_t)1 << 28)
extern uint32_t currentUIMode;
struct ProbeSharedCall {
	deluge::hid::Button button;
	bool on;
	bool inCardRoutine;
};
class ProbeView {
public:
	ActionResult result = ActionResult::NOT_DEALT_WITH;
	std::vector<ProbeSharedCall> calls;
	std::vector<void*> targets;
	ActionResult buttonAction(deluge::hid::Button button, bool on, bool inCardRoutine) {
		calls.push_back({button, on, inCardRoutine});
		return result;
	}
	// Exact pointer log for the shared modulation-target boundary. The real API
	// refreshes effect LED/knob indicators; no effect processing is simulated here.
	void setActiveModControllableTimelineCounter(TimelineCounter* target, bool = true) { targets.push_back(target); }
	void setActiveModControllableTimelineCounter(AudioClip* target, bool = true) { targets.push_back(target); }
	void setActiveModControllableTimelineCounter(std::nullptr_t, bool = true) { targets.push_back(nullptr); }
	void reset() {
		calls.clear();
		targets.clear();
		result = ActionResult::NOT_DEALT_WITH;
	}
};
extern ProbeView view;

#include "real_canvas.h"

// Probe declarations avoid firmware RootUI dependencies. Firmware compilation separately
// checks production declarations; all method bodies are extracted verbatim.
class LoopStationView : public ProbeUI {
public:
	bool opened() override;
	void focusRegained() override;
	void graphicsRoutine() override;
	ActionResult timerCallback();
	ActionResult padAction(int32_t, int32_t, int32_t);
	ActionResult buttonAction(deluge::hid::Button, bool, bool);
	bool getAffectEntire() override;
	UIModControllableContext getUIModControllableContext() override;
	bool renderMainPads(uint32_t, RGB[][kDisplayWidth + kSideBarWidth], uint8_t[][kDisplayWidth + kSideBarWidth],
	                    bool) override;
	bool renderSidebar(uint32_t, RGB[][kDisplayWidth + kSideBarWidth],
	                   uint8_t[][kDisplayWidth + kSideBarWidth]) override;
	void selectEncoderAction(int8_t);
	bool prepareTrack(loop_station::SlotId);
	void updateModTarget();
	void updateLed();
	void attemptExit();
	loop_station::RestoreId pendingRestore_{};
	loop_station::SlotId pendingRestoreSlot_ = loop_station::kInvalidSlot;
	bool pendingExit_ = false;
	bool modTargetIsClip_ = false;
};
