#pragma once
#include <array>
#include <cstdint>

namespace deluge::hid {
using Button = uint8_t;
namespace button {
enum : Button {
	AFFECT_ENTIRE = 1,
	CROSS_SCREEN_EDIT,
	SESSION_VIEW,
	CLIP_VIEW,
	SELECT_ENC,
	BACK,
	SAVE,
	LOAD,
	PLAY,
	RECORD,
	SHIFT,
	SYNTH,
	KIT,
	MIDI,
	CV,
	KEYBOARD
};
}
} // namespace deluge::hid
enum class ActionResult { NOT_DEALT_WITH, DEALT_WITH, REMIND_ME_OUTSIDE_CARD_ROUTINE };
enum class IndicatorLED { SAVE, LOAD, SESSION_VIEW };
enum class Status { Ok, Busy, Unavailable };
using SlotId = uint8_t;
struct RestoreId {
	bool valid() const { return false; }
};
inline bool validSlot(uint8_t slot) {
	return slot < 8;
}
inline constexpr int UI_MODE_NONE = 0;
inline constexpr int UI_MODE_HOLDING_SAVE_BUTTON = 51;
inline constexpr int UI_MODE_HOLDING_LOAD_BUTTON = 52;
inline constexpr int UI_MODE_AUDITIONING = 9;
inline constexpr int kShortPressTime = 1000;
inline constexpr int kDisplayWidth = 16;
inline constexpr int kSideBarWidth = 2;
inline constexpr int kDisplayHeight = 8;
inline int currentUIMode = UI_MODE_NONE;
inline uint32_t timeSaveButtonPressed = 0;
inline bool sdRoutineLock = false;
namespace AudioEngine {
inline uint32_t audioSampleTimer = 1;
}
enum class TimerName { UI_SPECIFIC };
struct TimerManager {
	void unsetTimer(TimerName) {}
};
inline TimerManager uiTimerManager{};
struct PadLEDs {
	static void reassessGreyout() {}
};

// The real UI base and navigation-hierarchy boundary mirror production ui.cpp
// semantics: openUI pushes the new UI before opened() and pops it on failure.
struct UI {
	virtual ~UI() = default;
	virtual UI* getUI() { return this; }
	virtual bool opened() { return true; }
	virtual void focusRegained() {}
	virtual ActionResult padAction(int32_t, int32_t, int32_t) { return ActionResult::DEALT_WITH; }
};
constexpr size_t kUiNavigationHistoryLength = 16;
inline std::array<UI*, kUiNavigationHistoryLength> uiNavigationHierarchy{};
inline int32_t numUIsOpen = 0;
inline UI* lastUIBeforeNullifying = nullptr;
inline UI* getCurrentUI() {
	if (numUIsOpen == 0)
		return lastUIBeforeNullifying;
	return uiNavigationHierarchy[numUIsOpen - 1];
}
inline UI* getRootUI() {
	if (numUIsOpen == 0)
		return nullptr;
	return uiNavigationHierarchy[0];
}
inline void renderUIsForOled() {
}
bool openUI(UI* ui);

// Narrow modal boundary. opened() is the failure-injection point: the test callback
// delivers the real physical matrix release while this modal is current, then
// reports whether opening succeeded. Only ownership/rollback is modeled here.
struct SaveSongUI : UI {
	bool (*openedCallback)(SaveSongUI&) = nullptr;
	bool opened() override { return openedCallback ? openedCallback(*this) : true; }
	unsigned padCalls = 0;
	unsigned releases = 0;
	ActionResult padAction(int32_t, int32_t, int32_t velocity) override {
		++padCalls;
		if (velocity == 0)
			++releases;
		return ActionResult::DEALT_WITH;
	}
};
inline SaveSongUI saveSongUI{};
struct LoadUI {
	virtual ~LoadUI() = default;
	virtual ActionResult padAction(int32_t, int32_t, int32_t) {
		++baseCalls;
		return ActionResult::DEALT_WITH;
	}
	unsigned baseCalls = 0;
};
struct LoadSongUI : LoadUI, UI {
	bool (*openedCallback)(LoadSongUI&) = nullptr;
	bool opened() override { return openedCallback ? openedCallback(*this) : true; }
	ActionResult padAction(int32_t x, int32_t y, int32_t on) override;
	bool qwertyVisible = true;
	int favouriteRow = 1;
	unsigned displayTextCalls = 0;
	void displayText(bool) { ++displayTextCalls; }
	void* getCurrentFileItem() const { return nullptr; }
	void drawKeys() {}
};
inline LoadSongUI loadSongUI{};
struct StemExport {
	bool processStarted = false;
};
extern StemExport stemExport;
class MatrixDriver {
public:
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity);
	bool padStates[kDisplayWidth + kSideBarWidth][kDisplayHeight]{};
};
extern MatrixDriver matrixDriver;

inline UI performanceView{};
namespace context_menu {
struct ClearSong : UI {
	bool setupAndCheckAvailability() { return true; }
};
inline ClearSong clearSong{};
} // namespace context_menu
struct ProbeDisplay {
	bool haveOLED() const { return false; }
	void displayPopup(const char*) {}
};
extern ProbeDisplay* display;
namespace indicator_leds {
inline void setLedState(IndicatorLED, bool) {
}
} // namespace indicator_leds
namespace deluge::l10n {
enum class String { STRING_FOR_CANT_SAVE_WHILE_OVERDUBS_PENDING };
inline const char* get(String) {
	return "stub";
}
} // namespace deluge::l10n
struct Song {
	bool affectEntire = false;
	struct SongModel* model = nullptr;
	bool hasAnyPendingNextOverdubs() const { return false; }
	SongModel* loopStation() { return model; }
};
extern Song* currentSong;
namespace Buttons {
inline bool pressed[256]{};
inline bool isButtonPressed(deluge::hid::Button button) {
	return pressed[button];
}
inline bool isShiftButtonPressed() {
	return pressed[deluge::hid::button::SHIFT];
}
} // namespace Buttons
struct SongModel {
	bool held = true;
	unsigned cancellations = 0;
	struct Engine {
		uint8_t selected() const { return 255; }
	} engine_;
	Engine& engine() { return engine_; }
	void* clip(SlotId) const { return nullptr; }
	bool canUndo(uint8_t) const { return false; }
	Status requestRestore(uint8_t, bool, RestoreId&) { return Status::Unavailable; }
	void cancelHeldPadGestures() {
		held = false;
		++cancellations;
	}
};
inline SongModel* model() {
	return currentSong ? currentSong->loopStation() : nullptr;
}
inline bool sharedExclusiveModeActive() {
	return false;
}
namespace indicator_leds {
inline void blinkLed(IndicatorLED) {
}
} // namespace indicator_leds
namespace deluge::gui::menu_item::loop_station {
struct SettingsMenu {
	bool openGlobal() { return true; }
};
inline SettingsMenu loopStationSettingsMenu{};
} // namespace deluge::gui::menu_item::loop_station
struct SessionView : UI {
	void transitionToViewForClip(void*) {}
};
extern SessionView sessionView;
struct View {
	ActionResult buttonAction(deluge::hid::Button button, bool on, bool inCardRoutine);
};
extern View view;
struct LoopStationView : UI {
	ActionResult buttonAction(deluge::hid::Button, bool, bool);
	void focusRegained() override;
	ActionResult padAction(int32_t, int32_t, int32_t velocity) override {
		if (velocity == 0)
			++padReleases;
		else
			++padPresses;
		return ActionResult::DEALT_WITH;
	}
	void attemptExit() {}
	void updateLed() {}
	void updateModTarget() {
		refreshed = true;
		cancellationsAtRefresh = model() ? model()->cancellations : 0;
	}
	void finishStutterBeforeTargetChange() {}
	RestoreId pendingRestore_{};
	uint8_t pendingRestoreSlot_ = 255;
	unsigned padPresses = 0;
	unsigned padReleases = 0;
	bool refreshed = false;
	unsigned cancellationsAtRefresh = 0;
};
