#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace deluge::hid {
using Button = uint8_t;
namespace button {
enum : Button {
	AFFECT_ENTIRE = 1,
	BACK,
	CROSS_SCREEN_EDIT,
	SELECT_ENC,
	PLAY,
	SHIFT,
	RECORD,
	TEMPO_ENC,
	TAP_TEMPO,
	MOD_ENCODER_0,
	MOD_ENCODER_1,
	SESSION_VIEW,
	CLIP_VIEW,
	SAVE,
	LOAD,
	SYNTH,
	KIT,
	MIDI,
	CV,
	KEYBOARD,
	LEARN
};
struct XY {
	unsigned x;
	unsigned y;
};
inline XY toXY(Button b) {
	return {static_cast<unsigned>(b % 9), static_cast<unsigned>(b % 8)};
}
} // namespace button
} // namespace deluge::hid
enum class ActionResult { NOT_DEALT_WITH, DEALT_WITH, REMIND_ME_OUTSIDE_CARD_ROUTINE };
constexpr int NUM_BUTTON_COLS = 9, NUM_BUTTON_ROWS = 8;
constexpr int kNumModButtons = 0;
constexpr int modButtonX[1]{}, modButtonY[1]{};
constexpr int LONG_PRESS_DURATION = 100, kShortPressTime = 1000, kInternalButtonPressLatency = 0;
enum class TimerName { BACK_MENU_EXIT };
enum class PopupType { THRESHOLD_RECORDING_MODE };
enum class RuntimeFeatureSettingType { EmulatedDisplay };
enum class RuntimeFeatureStateEmulatedDisplay { Hardware };
enum class AudioInputChannel { NONE, ACTIVE };
inline bool operator>(AudioInputChannel left, AudioInputChannel right) {
	return static_cast<int>(left) > static_cast<int>(right);
}
struct ProbeDisplay {
	void cancelPopup() {}
	bool hasPopupOfType(PopupType) const { return false; }
	bool haveOLED() const { return false; }
	bool have7SEG() const { return true; }
	void displayPopup(const char*) {}
};
extern ProbeDisplay* display;
struct UI {
	virtual ~UI() = default;
	virtual ActionResult buttonAction(deluge::hid::Button, bool, bool) {
		++buttonCalls;
		return result;
	}
	virtual void modEncoderButtonAction(uint8_t, bool on) {
		++encoderCalls;
		encoderEdges.emplace_back(on, true);
	}
	virtual void modButtonAction(int, bool) {}
	unsigned buttonCalls = 0, encoderCalls = 0;
	std::vector<std::pair<bool, bool>> encoderEdges;
	ActionResult result = ActionResult::NOT_DEALT_WITH;
};
extern UI* currentUI;
extern UI* rootUI;
inline unsigned getCurrentUICalls = 0;
inline UI* getCurrentUI() {
	++getCurrentUICalls;
	return currentUI;
}
struct View {
	bool pendingStutterEncoderRelease_ = false;
	bool consumePendingStutterEncoderRelease(uint8_t which, bool on);
};
extern View view;
struct TimerManager {
	void setTimer(TimerName, int) {}
	void unsetTimer(TimerName) {}
};
extern TimerManager uiTimerManager;
struct RuntimeSettings {
	RuntimeFeatureStateEmulatedDisplay get(RuntimeFeatureSettingType) const {
		return RuntimeFeatureStateEmulatedDisplay::Hardware;
	}
};
extern RuntimeSettings runtimeFeatureSettings;
struct AudioRecorder {
	AudioInputChannel recordingSource = AudioInputChannel::NONE;
	void beginOutputRecording() {}
	bool isCurrentlyResampling() const { return false; }
	void endRecordingSoon(int) {}
};
extern AudioRecorder audioRecorder;
struct PlaybackHandler {
	bool playbackState = false;
	bool isEitherClockActive() const { return false; }
	void stopOutputRecordingAtLoopEnd() {}
	void playButtonPressed(int) {}
	void beginOutputRecording() {}
	void recordButtonPressed() {}
	void commandClearTempoAutomation() {}
	void commandDisplaySwingInterval() {}
	void commandDisplayTempo() {}
};
extern PlaybackHandler playbackHandler;
struct PlaybackMode {
	void stopOutputRecordingAtLoopEnd() {}
};
extern PlaybackMode* currentPlaybackMode;
namespace AudioEngine {
extern uint32_t audioSampleTimer;
}
namespace FlashStorage {
constexpr uint32_t holdTime = 100;
}
namespace deluge::hid::display {
inline void swapDisplayType() {
}
} // namespace deluge::hid::display
inline constexpr int RuntimeFeatureStateToggle = 0;
struct LoadUI {
	bool isLoadingSong() const { return false; }
};
extern LoadUI loadSongUI;
struct RootViews {};
extern UI sessionView, arrangerView;

namespace Buttons {
extern bool recordButtonPressUsedUp, selectButtonPressUsedUp;
extern uint32_t timeRecordButtonPressed;
extern bool shiftCurrentlyPressed, considerShiftReleaseForSticky, considerCrossScreenReleaseForCrossScreenMode;
extern bool buttonStates[NUM_BUTTON_COLS + 1][NUM_BUTTON_ROWS];
inline bool isShiftButtonPressed() {
	return shiftCurrentlyPressed;
}
inline bool isButtonPressed(deluge::hid::Button button) {
	const auto xy = deluge::hid::button::toXY(button);
	return buttonStates[xy.x][xy.y];
}
void commandToggleShift(bool);
ActionResult buttonAction(deluge::hid::Button, bool, bool);
void dispatchModEncoderButtonAction(uint8_t, bool);
} // namespace Buttons
enum class MenuField { CopyToTrack, Other };
class HorizontalMenuStub {
public:
	MenuField selectedField() const { return MenuField::Other; }
	void renderOLED() {}
	void buttonAction(deluge::hid::Button, bool, bool) {}
};
bool shouldCloseSettingsMenu(bool, bool, bool);
extern bool sdRoutineLock;
class SettingsMenu : public UI {
public:
	ActionResult buttonAction(deluge::hid::Button, bool, bool) override;
	unsigned handlerCalls = 0;
	bool selectHeld_ = false, copyReview_ = false;
	HorizontalMenuStub horizontalMenu_{};
	bool useHorizontalMenu() const { return true; }
	void confirmCopy() {}
	void requestCopyReview() {}
	void selectCurrent() {}
	void cancelCopyReview() {}
	void closeMenu() { currentUI = rootUI; }
	void refreshDisplay() {}
};
