#pragma once

#include "definitions_cxx.hpp"
#include "deluge/loop_station/types.h"
#include "gui/ui/root_ui.h"
#include "hid/button.h"

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
	UIType getUIType() override { return UIType::LOOP_STATION; }

private:
	loop_station::RestoreId pendingRestore_{};
	loop_station::SlotId pendingRestoreSlot_ = loop_station::kInvalidSlot;
	bool pendingExit_ = false;
	void updateLed();
	void attemptExit();
};

extern LoopStationView loopStationView;
