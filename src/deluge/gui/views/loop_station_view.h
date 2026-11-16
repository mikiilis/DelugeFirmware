#pragma once

#include "definitions_cxx.hpp"
#include "deluge/loop_station/history_navigation.h"
#include "deluge/loop_station/types.h"
#include "gui/ui/root_ui.h"
#include "hid/button.h"

class NumericLayerScrollingText;

class LoopStationView final : public RootUI {
public:
	bool opened() override;
	void focusRegained() override;
	void graphicsRoutine() override;
	ActionResult timerCallback() override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	ActionResult buttonAction(deluge::hid::Button button, bool on, bool inCardRoutine) override;
	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea = true) override;
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) override;
	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override;
	void selectEncoderAction(int8_t offset) override;
	void modEncoderAction(int32_t whichModEncoder, int32_t offset) override;
	void modButtonAction(uint8_t whichButton, bool on) override;
	void modEncoderButtonAction(uint8_t whichModEncoder, bool on) override;
	void noteUnrelatedEncoderAction() override;
	UIType getUIType() override { return UIType::LOOP_STATION; }
	bool getAffectEntire() override;
	UIModControllableContext getUIModControllableContext() override;

private:
	// View-side confirmed history prompt. The model owns any accepted job; this
	// only tracks the two-press confirmation and the observed terminal result.
	enum class HistoryPromptState : uint8_t { None, Confirm, Applying, Result };
	HistoryPromptState historyPromptState_ = HistoryPromptState::None;
	loop_station::HistoryTarget historyTarget_{};
	loop_station::RestoreId historyId_{};
	loop_station::Status historyStatus_ = loop_station::Status::Ok;
	bool historyBackDown_ = false;
	// Owned seven-segment scrolling feedback. Ownership is recorded only after an
	// overlay install succeeds; content changes reuse neither the old layer nor its
	// scroll position.
	char historyFeedbackText_[64] = {};
	bool historyFeedbackOwned_ = false;
	NumericLayerScrollingText* historyFeedbackLayer_ = nullptr;
	bool pendingExit_ = false;
	bool modTargetIsClip_ = false;
	void cancelHistoryConfirmation();
	void clearHistoryFeedback();
	void handleHistoryPress(bool redo);
	void observeHistoryResult();
	void refreshHistoryFeedback();
	bool formatHistoryFeedback(char* text, size_t size);
	bool installHistoryFeedback(char const* text);
	bool historyFeedbackVisible();
	void updateLed();
	void updateModTarget();
	void finishStutterBeforeTargetChange();
	void attemptExit();
	bool prepareTrack(loop_station::SlotId slot);
};

extern LoopStationView loopStationView;
