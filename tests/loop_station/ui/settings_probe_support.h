#pragma once
#include "horizontal_settings_declarations.h"
#include "settings_items_declarations.h"
#include "shared_probe_support.h"

namespace deluge::gui::menu_item::loop_station {
class SettingsMenu : public ProbeUI {
public:
	bool openGlobal();
	bool openTrack(::loop_station::SlotId);
	bool opened() override;
	bool useHorizontalMenu() const;
	void requestCopyReview();
	void confirmCopy();
	void cancelCopyReview();
	ActionResult buttonAction(deluge::hid::Button, bool, bool);
	ActionResult padAction(int32_t, int32_t, int32_t);
	ActionResult horizontalEncoderAction(int32_t);
	void selectEncoderAction(int8_t);
	void renderOLED(deluge::hid::display::oled_canvas::Canvas&);
	void renderListOLED(deluge::hid::display::oled_canvas::Canvas&);
	bool open(::loop_station::MenuScope, ::loop_station::SlotId);
	void changeValue(int32_t);
	void selectCurrent();
	void closeMenu();
	void refreshDisplay();
	SettingsContext context_{};
	SettingsItems items_{context_};
	SettingsHorizontalMenu horizontalMenu_{context_, items_};
	bool horizontalSession_ = false;
	bool copyReview_ = false;
	bool selectHeld_ = false;
	::loop_station::SlotId copyReviewTarget_ = 1;
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
