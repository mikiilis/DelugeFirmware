/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "gui/ui/root_ui.h"
#include "loop_station/loop_station.h"
#include "model/clip/clip_minder.h"
#include "util/d_string.h"

#include <array>
#include <optional>

class AudioClip;
enum class RecordingMode;

class LoopStationView final : public RootUI, public ClipMinder, private deluge::loop_station::EngineDelegate {
public:
	bool opened() override;
	void focusRegained() override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	ActionResult buttonAction(deluge::hid::Button button, bool on, bool inCardRoutine) override;
	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea = true) override;
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) override;
	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override;
	void graphicsRoutine() override;
	void playbackEnded() override;
	void processMusicalTick();
	void applyTrackSource(deluge::loop_station::SlotId slot);
	void refreshMonitoring();
	UIType getUIType() override { return UIType::LOOP_STATION; }
	UIModControllableContext getUIModControllableContext() override {
		return selectedSlot_ ? UIModControllableContext::CLIP : UIModControllableContext::SONG;
	}
	ClipMinder* toClipMinder() override { return clipMenuContext_ ? this : nullptr; }

private:
	deluge::loop_station::Transport transport() const;
	uint64_t musicalPositionQ16() const;
	uint8_t capturingMask() const;
	void updateRecordingMode();
	void updateUndoLed();
	void updateModControllableTarget();
	void dispatchCentreGesture(deluge::loop_station::SlotId slot, uint32_t pressSample);
	void requestExit();
	void disableMonitoring();
	void restoreBoundPhrases();
	AudioClip* backingClip(deluge::loop_station::SlotId slot) const;
	AudioClip* ensureBackingClip(deluge::loop_station::SlotId slot);
	void clearBackingClip(deluge::loop_station::SlotId slot);
	RGB palette(deluge::loop_station::PadColour colour) const;
	RGB destinationColour(deluge::loop_station::PendingAction action) const;
	bool preflight(deluge::loop_station::SlotId slot, bool overdub) override;
	bool beginCapture(deluge::loop_station::SlotId slot, bool overdub, uint32_t tick) override;
	deluge::loop_station::CaptureFinish finishCapture(deluge::loop_station::SlotId slot, bool overdub, uint32_t length,
	                                                  bool crop) override;
	void abortCapture(deluge::loop_station::SlotId slot, bool overdub) override;
	void phraseCleared(deluge::loop_station::SlotId slot) override;
	bool navigateHistory(deluge::loop_station::SlotId slot, bool redo, bool& targetHasPhrase) override;
	void stateChanged(deluge::loop_station::SlotId slot, deluge::loop_station::TrackState state) override;

	std::optional<deluge::loop_station::Engine> engine_;
	std::array<uint8_t, deluge::loop_station::kTrackCount> centreHeldMask_{};
	std::array<uint16_t, deluge::loop_station::kTrackCount> borderHeldMask_{};
	std::array<bool, deluge::loop_station::kTrackCount> centreGestureActive_{};
	std::array<bool, deluge::loop_station::kTrackCount> centreGestureDispatched_{};
	std::array<uint32_t, deluge::loop_station::kTrackCount> centrePressSample_{};
	std::array<uint32_t, deluge::loop_station::kTrackCount> captureOriginalLengths_{};
	std::optional<deluge::loop_station::SlotId> selectedSlot_;
	uint8_t monitoringCaptureMask_ = 0;
	RecordingMode recordingModeBefore_{};
	bool exitRequested_ = false;
	bool clipMenuContext_ = false;
	bool monitoringActive_ = false;
};

extern LoopStationView loopStationView;
