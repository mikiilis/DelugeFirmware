#pragma once

#include "deluge/loop_station/menu_model.h"
#include "gui/ui/root_ui.h"

namespace deluge::gui::menu_item::loop_station {

namespace ls = ::loop_station;

class SettingsMenu final : public RootUI {
public:
	bool openGlobal();
	bool openTrack(ls::SlotId slot);
	bool opened() override;
	ActionResult buttonAction(deluge::hid::Button button, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	ActionResult horizontalEncoderAction(int32_t offset) override;
	void selectEncoderAction(int8_t offset) override;
	bool renderMainPads(uint32_t rows, RGB image[][kDisplayWidth + kSideBarWidth],
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea = true) override;
	bool renderSidebar(uint32_t rows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) override;
	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override;
	UIType getUIType() override { return UIType::LOOP_STATION_SETTINGS; }

private:
	bool open(ls::MenuScope scope, ls::SlotId slot);
	void changeValue(int32_t offset);
	void selectCurrent();
	void closeMenu();
	void refreshDisplay();
	ls::MenuFields fields_{};
	ls::MenuScope scope_ = ls::MenuScope::Global;
	ls::SlotId slot_ = 0;
	ls::SlotId copyTarget_ = 1;
	uint8_t row_ = 0;
	bool editing_ = false;
	bool choosingCopyTarget_ = false;
};

extern SettingsMenu loopStationSettingsMenu;

} // namespace deluge::gui::menu_item::loop_station
