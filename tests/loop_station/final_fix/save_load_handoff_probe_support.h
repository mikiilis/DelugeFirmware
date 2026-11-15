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
inline int currentUIMode = UI_MODE_NONE;
inline uint32_t timeSaveButtonPressed = 0;
namespace AudioEngine {
inline uint32_t audioSampleTimer = 1;
}
struct UI {
	virtual ~UI() = default;
};
extern UI* currentUI;
inline UI* getCurrentUI() {
	return currentUI;
}
extern UI* rootUI;
inline UI* getRootUI() {
	return rootUI;
}
inline UI performanceView{}, saveSongUI{}, loadSongUI{};
inline bool openUI(UI* ui) {
	currentUI = ui;
	return true;
}
namespace context_menu {
struct ClearSong : UI {
	bool setupAndCheckAvailability() { return true; }
};
inline ClearSong clearSong{};
} // namespace context_menu
struct ProbeDisplay {
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
	void attemptExit() {}
	void updateModTarget() {}
	void finishStutterBeforeTargetChange() {}
	RestoreId pendingRestore_{};
	uint8_t pendingRestoreSlot_ = 255;
};
