#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace deluge::hid {
using Button = uint8_t;
namespace button {
inline constexpr Button CLIP_VIEW = 7;
}
} // namespace deluge::hid
enum class ActionResult { NOT_DEALT_WITH, DEALT_WITH };
struct TimelineCounter;
inline unsigned probeSequence = 0;
inline unsigned sourceRestoreSequence = 0;
inline unsigned destinationSetupSequence = 0;
struct ModControllableAudio {
	void* expectedParamManager = nullptr;
	int32_t* rate = nullptr;
	int32_t savedRate = 0;
	void* receivedParamManager = nullptr;
	void endStutter(void* manager) {
		receivedParamManager = manager;
		if (rate)
			*rate = savedRate;
		sourceRestoreSequence = ++probeSequence;
	}
};
using ParamManagerForTimeline = void;
struct ModelStackWithTimelineCounter {
	void* timelineCounter = nullptr;
	void* modControllable = nullptr;
	void* paramManager = nullptr;
	bool timelineCounterIsSet() const { return timelineCounter != nullptr; }
	TimelineCounter* getTimelineCounter();
	ModelStackWithTimelineCounter* addTimelineCounter(void* value) {
		timelineCounter = value;
		return this;
	}
	void setTimelineCounter(void* value) { timelineCounter = value; }
	void addOtherTwoThingsButNoNoteRow(void* mod, void* params) {
		modControllable = mod;
		paramManager = params;
	}
};
struct ModelStackWithThreeMainThings : ModelStackWithTimelineCounter {};
struct Song;
enum class SessionLayoutType { Other, SessionLayoutTypeGrid };
using SlotId = uint8_t;
constexpr SlotId kInvalidSlot = 255;
inline bool validSlot(SlotId slot) {
	return slot < 8;
}
struct TimelineCounter {
	virtual ~TimelineCounter() = default;
	virtual TimelineCounter* getTimelineCounterToRecordTo() { return this; }
	virtual void getActiveModControllable(ModelStackWithTimelineCounter*) = 0;
};
inline TimelineCounter* ModelStackWithTimelineCounter::getTimelineCounter() {
	return static_cast<TimelineCounter*>(timelineCounter);
}
struct AudioClip : TimelineCounter {
	int mod = 5;
	int params = 6;
	struct SampleHolder {
		void* audioFile = nullptr;
	} sampleHolder;
	void getActiveModControllable(ModelStackWithTimelineCounter* stack) override {
		stack->setTimelineCounter(this);
		stack->addOtherTwoThingsButNoNoteRow(&mod, &params);
	}
};
struct Engine {
	SlotId selected() const { return selectedSlot; }
	SlotId selectedSlot = kInvalidSlot;
};
enum class Status { Ok, Busy, Unavailable };
struct SongModel {
	Engine engine_;
	AudioClip* boundClip = nullptr;
	Engine& engine() { return engine_; }
	AudioClip* clip(SlotId) const { return boundClip; }
	Status requestViewExit() { return Status::Ok; }
};
struct Song final : TimelineCounter {
	bool affectEntire = false;
	int globalEffectable = 1;
	int paramManager = 2;
	SongModel* model = nullptr;
	AudioClip* currentClip = nullptr;
	int lastClipInstanceEnteredStartPos = -1;
	SessionLayoutType sessionLayout = SessionLayoutType::Other;
	int32_t xScroll[4]{};
	int32_t xZoom[4]{};
	TimelineCounter* getTimelineCounterToRecordTo() override { return this; }
	SongModel* loopStation() { return model; }
	void setCurrentClip(AudioClip* clip) { currentClip = clip; }
	void getActiveModControllable(ModelStackWithTimelineCounter*) override;
	ModelStackWithThreeMainThings* setupModelStackWithSongAsTimelineCounter(void*);
};
inline ModelStackWithThreeMainThings* setupModelStackWithSong(ModelStackWithThreeMainThings* stack, Song*) {
	return stack;
}
inline ModelStackWithThreeMainThings* setupModelStackWithThreeMainThingsButNoNoteRow(void* memory,
                                                                                     TimelineCounter* timelineCounter,
                                                                                     void* mod, TimelineCounter*,
                                                                                     void* params) {
	auto* stack = static_cast<ModelStackWithThreeMainThings*>(memory);
	stack->timelineCounter = timelineCounter;
	stack->addOtherTwoThingsButNoNoteRow(mod, params);
	return stack;
}
extern Song* currentSong;
extern bool renderedVUMeter;
inline uint32_t currentUIMode = 0;
inline constexpr uint32_t UI_MODE_STUTTERING = uint32_t{1} << 28;
inline constexpr uint32_t UI_MODE_NONE = 0;
inline constexpr uint32_t UI_MODE_ANIMATION_FADE = 2;
inline constexpr uint32_t UI_MODE_CLIP_PRESSED_IN_SONG_VIEW = 4;
inline bool isUIModeActive(uint32_t flag) {
	return currentUIMode & flag;
}
inline uint8_t selectedClipYDisplay = 0;
inline void pretendModKnobsUntouchedForAWhile() {
	destinationSetupSequence = ++probeSequence;
}
inline void setModLedStates() {
}
inline void setKnobIndicatorLevels() {
}
inline bool rootUIIsClipMinderScreen() {
	return false;
}
inline void uiNeedsRendering(void*, int) {
}
inline void* getRootUI() {
	return nullptr;
}
inline void sendMidiFollowFeedback() {
}

class View {
public:
	ModelStackWithThreeMainThings activeModControllableModelStack{};
	bool pendingStutterEncoderRelease_ = false;
	void setActiveModControllableTimelineCounter(TimelineCounter*, bool = true, bool = false);
	bool finishActiveStutterForModTargetChange();
	void focusRegained() {}
	void displayOutputName(void*, bool) {}
};
extern View view;
inline SongModel* model() {
	return currentSong ? currentSong->loopStation() : nullptr;
}
struct LoopStationView {
	bool modTargetIsClip_ = false;
	bool pendingExit_ = false;
	void updateModTarget();
	void finishStutterBeforeTargetChange();
	void attemptExit();
	ActionResult clipViewNavigation(deluge::hid::Button, bool, bool);
};

enum class IndicatorLED { BACK, SESSION_VIEW };
namespace indicator_leds {
inline void setLedState(IndicatorLED, bool) {
}
inline void blinkLed(IndicatorLED) {
}
} // namespace indicator_leds
struct DisplayProbe {
	bool haveOLED() const { return true; }
	bool have7SEG() const { return false; }
	void displayPopup(const char*) {}
};
extern DisplayProbe* display;
struct ClipNavigationTimelineView {
	void focusRegained() {}
};
struct SessionView : ClipNavigationTimelineView {
	bool viewingRecordArmingActive = false;
	bool horizontalEncoderPressed = false;
	bool gridSelectedClipPulsing = false;
	void selectLayout(int) {}
	void redrawClipsOnScreen(bool) {}
	void setCentralLEDStates() {}
	void redrawNumericDisplay() {}
	void setLedStates() {}
	void gridPulseSelectedClip() { gridSelectedClipPulsing = true; }
	void transitionToViewForClip(AudioClip* clip);
	void gridTransitionToViewForClip(AudioClip*) {}
	void focusRegained();
};
struct ClipView {
	void focusRegained() {}
};
struct AudioClipView : ClipView {
	AudioClip* currentClip = nullptr;
	bool endMarkerVisible = true;
	bool startMarkerVisible = true;
	AudioClip* getCurrentClip() { return currentSong ? currentSong->currentClip : currentClip; }
	void* getCurrentOutput() { return nullptr; }
	void focusRegained();
};
extern SessionView sessionView;
extern AudioClipView audioClipView;
extern void* currentRoot;
inline void changeRootUI(void* ui) {
	currentRoot = ui;
}
inline constexpr int NAVIGATION_CLIP = 1;
inline constexpr int kDisplayHeight = 8;
constexpr int32_t operator""_i32(unsigned long long value) {
	return static_cast<int32_t>(value);
}
inline int32_t getClipPlaceOnScreen(AudioClip*) {
	return 3;
}
inline int32_t getClipLocalScroll(AudioClip*, int32_t scroll, int32_t) {
	return scroll;
}
inline AudioClip* getCurrentAudioClip() {
	return currentSong ? currentSong->currentClip : nullptr;
}
struct Sample {};
inline int UI_MODE_AUDIO_CLIP_EXPANDING = 8;
inline int kClipCollapseSpeed = 0;
struct WaveformRenderer {
	int collapseAnimationToWhichRow = 0;
};
extern WaveformRenderer waveformRenderer;
namespace PadLEDs {
inline void setupAudioClipCollapseOrExplodeAnimation(AudioClip*) {
}
inline void recordTransitionBegin(int) {
}
inline void renderAudioClipExpandOrCollapse() {
}
inline void clearSideBar() {
}
} // namespace PadLEDs
