#pragma once

#include "gui/menu_item/horizontal_menu.h"
#include "settings_items.h"

namespace deluge::gui::menu_item::loop_station {

class SettingsHorizontalMenu final : public HorizontalMenu {
public:
	SettingsHorizontalMenu(SettingsContext& context, SettingsItems& items);
	void resetSelection();
	ls::MenuField selectedField() const;
	void selectField(ls::MenuField field);
	void updatePadLights() override;

protected:
	bool isChildRelevant(MenuItem* item) const override;
	void beginHostSession(MenuItem* from) override;
	void endHostSession() override {}
	MenuPermission initializeItem(MenuItem* item) override;
	bool handleItemPermission(MenuItem*, MenuPermission permission) override;
	void handleItemAction(MenuItem*) override {} // Re-selection never activates Copy.
	bool trySwitchHorizontalMenu(int32_t) override { return false; }
	double calcNextKnobSpeed(int8_t offset) override;

private:
	SettingsContext& context_;
	SettingsItems& settingsItems_;
};
} // namespace deluge::gui::menu_item::loop_station
