#include "horizontal_settings.h"

#include "gui/l10n/strings.h"
#include "gui/ui/root_ui.h"
#include "gui/ui/ui.h"

namespace deluge::gui::menu_item::loop_station {

SettingsHorizontalMenu::SettingsHorizontalMenu(SettingsContext& context, SettingsItems& items)
    : HorizontalMenu(l10n::String::STRING_FOR_LOOP_STATION_GLOBAL_SETTINGS, items.all(), DYNAMIC), context_(context),
      settingsItems_(items) {
}

void SettingsHorizontalMenu::resetSelection() {
	current_item_ = items.end();
	lastSelectedItemPosition = kNoSelection;
	title = context_.scope == ls::MenuScope::Global ? l10n::String::STRING_FOR_LOOP_STATION_GLOBAL_SETTINGS
	                                                : l10n::String::STRING_FOR_LOOP_STATION_TRACK_SETTINGS;
	focusChild(nullptr);
}

ls::MenuField SettingsHorizontalMenu::selectedField() const {
	return settingsItems_.field(getCurrentItem());
}

void SettingsHorizontalMenu::selectField(ls::MenuField field) {
	focusChild(settingsItems_.item(field));
}

bool SettingsHorizontalMenu::isChildRelevant(MenuItem* item) const {
	return context_.relevant(settingsItems_.field(item));
}

void SettingsHorizontalMenu::beginHostSession(MenuItem* from) {
	focusChild(from);
}

MenuPermission SettingsHorizontalMenu::initializeItem(MenuItem* item) {
	if (!isChildRelevant(item))
		return MenuPermission::NO;
	item->beginSession(nullptr);
	return MenuPermission::YES;
}

bool SettingsHorizontalMenu::handleItemPermission(MenuItem*, MenuPermission permission) {
	return permission == MenuPermission::YES;
}

void SettingsHorizontalMenu::updatePadLights() {
	uiNeedsRendering(getRootUI());
}

double SettingsHorizontalMenu::calcNextKnobSpeed(int8_t offset) {
	return selectedField() == ls::MenuField::Transpose ? 1 : HorizontalMenu::calcNextKnobSpeed(offset);
}
} // namespace deluge::gui::menu_item::loop_station
