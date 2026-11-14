#include "CppUTest/TestHarness.h"
#include "shared_probe_support.h"
using namespace deluge::gui::menu_item;
using namespace deluge::hid::button;

namespace {
struct ExternalMenu : HorizontalMenu {
	using HorizontalMenu::HorizontalMenu;
	int begins = 0, ends = 0, initializations = 0, actions = 0, chains = 0;
	void beginHostSession(MenuItem* from) override {
		++begins;
		focusChild(from);
	}
	void endHostSession() override { ++ends; }
	bool isChildRelevant(MenuItem* item) const override { return item->relevant; }
	MenuPermission initializeItem(MenuItem*) override {
		++initializations;
		return MenuPermission::YES;
	}
	void handleItemAction(MenuItem*) override { ++actions; }
	bool trySwitchHorizontalMenu(int32_t) override {
		++chains;
		return false;
	}
	double calcNextKnobSpeed(int8_t) override { return 1; }
	void updatePadLights() override {}
	using HorizontalMenu::paging;
	using HorizontalMenu::switchVisiblePage;
};
void press(HorizontalMenu& menu, deluge::hid::Button button) {
	probeMenuTime += 1;
	menu.buttonAction(button, true, false);
}
} // namespace
TEST_GROUP(SharedHorizontalMenu) {
	ProbeDisplay screen;
	void setup() {
		display = &screen;
		soundEditor = {};
		probeChain.clear();
		Buttons::shiftPressed = false;
		Buttons::selectPressed = Buttons::selectButtonPressUsedUp = false;
		indicator_leds::states.clear();
		FlashStorage::accessibilityMenuHighlighting = MenuHighlighting::FULL_INVERSION;
		deluge::hid::display::OLED::main = {};
	}
	void teardown() {
		display = nullptr;
		probeChain.clear();
		Buttons::shiftPressed = Buttons::selectPressed = Buttons::selectButtonPressUsedUp = false;
		FlashStorage::accessibilityMenuHighlighting = MenuHighlighting::FULL_INVERSION;
	}
};
TEST(SharedHorizontalMenu, ExternalHostDoesNotTouchEditorState) {
	MenuItem a, b, c, d;
	Decimal decimal;
	ExternalMenu menu({}, {&a, &b, &c, &d, &decimal});
	MultiRange range;
	soundEditor.currentMultiRange = &range;
	soundEditor.currentMultiRangeIndex = 4;
	soundEditor.currentClip = &a;
	soundEditor.currentModControllable = &b;
	soundEditor.currentSourceIndex = 6;
	soundEditor.numberEditPos = 7;
	soundEditor.numberEditSize = 1000;
	soundEditor.numberScrollAmount = 3;
	soundEditor.navigationDepth = 9;
	soundEditor.currentParamShortcutX = 5;
	soundEditor.currentParamShortcutY = 6;
	menu.beginSession(nullptr);
	menu.renderOLED();
	press(menu, KIT);
	press(menu, KIT);
	Buttons::shiftPressed = true;
	press(menu, CROSS_SCREEN_EDIT);
	menu.renderOLED();
	menu.selectEncoderAction(1);
	menu.endSession();
	POINTERS_EQUAL(&range, soundEditor.currentMultiRange);
	CHECK_EQUAL(4, soundEditor.currentMultiRangeIndex);
	POINTERS_EQUAL(&a, soundEditor.currentClip);
	POINTERS_EQUAL(&b, soundEditor.currentModControllable);
	CHECK_EQUAL(6, soundEditor.currentSourceIndex);
	CHECK_EQUAL(7, soundEditor.numberEditPos);
	CHECK_EQUAL(1000, soundEditor.numberEditSize);
	CHECK_EQUAL(3, soundEditor.numberScrollAmount);
	CHECK_EQUAL(9, soundEditor.navigationDepth);
	CHECK_EQUAL(5, soundEditor.currentParamShortcutX);
	CHECK_EQUAL(6, soundEditor.currentParamShortcutY);
	CHECK_EQUAL(1, menu.begins);
	CHECK_EQUAL(1, menu.ends);
	CHECK_EQUAL(1, menu.actions);
	CHECK_EQUAL(1, menu.chains);
	CHECK_EQUAL(4, menu.initializations); // begin, select, page, edit refocus
	CHECK_EQUAL(0, a.relevanceCalls);
	CHECK_EQUAL(0, decimal.relevanceCalls);
}
TEST(SharedHorizontalMenu, HorizontalDecimalPreservesFractionAndCursorState) {
	Decimal decimal;
	ExternalMenu menu({}, {&decimal});
	menu.beginSession(nullptr);
	soundEditor.numberEditPos = 7;
	soundEditor.numberEditSize = 1000;
	soundEditor.numberScrollAmount = 3;
	menu.selectEncoderAction(1);
	CHECK_EQUAL(223, decimal.written);
	decimal.value = 123;
	Buttons::shiftPressed = true;
	menu.selectEncoderAction(1);
	CHECK_EQUAL(124, decimal.written);
	CHECK_EQUAL(7, soundEditor.numberEditPos);
	CHECK_EQUAL(1000, soundEditor.numberEditSize);
	CHECK_EQUAL(3, soundEditor.numberScrollAmount);
}
TEST(SharedHorizontalMenu, DefaultHostRetainsEditorServices) {
	MenuItem item;
	item.range = true;
	HorizontalMenu menu({}, {&item});
	MultiRange range;
	soundEditor.currentMultiRange = &range;
	soundEditor.currentMultiRangeIndex = 2;
	soundEditor.currentModControllable = &item;
	soundEditor.currentSourceIndex = 6;
	menu.focusChild(&item);
	const int before = item.permissions;
	menu.beginSession(nullptr);
	CHECK_EQUAL(before + 1, item.permissions);
	POINTERS_EQUAL(&item, item.permissionMod);
	CHECK_EQUAL(6, item.permissionSource);
	POINTERS_EQUAL(&range, soundEditor.currentMultiRange);
	CHECK_EQUAL(2, soundEditor.currentMultiRangeIndex);
	menu.renderOLED();
	press(menu, SYNTH);
	CHECK_EQUAL(1, item.presses);
	item.enter = true;
	press(menu, SYNTH);
	CHECK_EQUAL(1, soundEditor.entries);
	POINTERS_EQUAL(&item, soundEditor.menuItemNavigationRecord[1]);
	menu.updatePadLights();
	CHECK_EQUAL(1, soundEditor.padUpdates);
	menu.endSession();
	CHECK_EQUAL(kNoSelection, soundEditor.currentParamShortcutX);
	CHECK_EQUAL(kNoSelection, soundEditor.currentParamShortcutY);
	POINTERS_EQUAL(nullptr, item.parent);
}
TEST(SharedHorizontalMenu, DefaultHostClearsRangeAndRoutesRangePermission) {
	MenuItem a, b;
	HorizontalMenu menu({}, {&a, &b});
	MultiRange range;
	soundEditor.currentMultiRange = &range;
	menu.beginSession(nullptr);
	POINTERS_EQUAL(nullptr, soundEditor.currentMultiRange);
	menu.renderOLED();
	b.permission = MenuPermission::MUST_SELECT_RANGE;
	press(menu, KIT);
	CHECK_EQUAL(1, b.permissions);
	POINTERS_EQUAL(&b, multiRangeMenu.menuItemHeadingTo);
	POINTERS_EQUAL(&multiRangeMenu, soundEditor.menuItemNavigationRecord[1]);
}
TEST(SharedHorizontalMenu, DefaultHostShiftChainsAndUnshiftedPagesLocally) {
	MenuItem a, b, c, d, e, target;
	HorizontalMenu first({}, {&a, &b, &c, &d, &e}), second({}, {&target});
	probeChain = {&first, &second};
	first.beginSession(nullptr);
	first.renderOLED();
	press(first, CROSS_SCREEN_EDIT);
	POINTERS_EQUAL(&e, first.getCurrentItem());
	CHECK_EQUAL(0, soundEditor.navigationDepth);
	Buttons::shiftPressed = true;
	press(first, CROSS_SCREEN_EDIT);
	POINTERS_EQUAL(&second, soundEditor.menuItemNavigationRecord[0]);
	POINTERS_EQUAL(&target, second.getCurrentItem());
	CHECK(target.permissions > 0);
}
TEST(SharedHorizontalMenu, AcceptedPressDispatchesOnceAndDebounceAndReleaseAreInert) {
	MenuItem a, b;
	ExternalMenu menu({}, {&a, &b});
	menu.beginSession(nullptr);
	menu.renderOLED();
	CHECK_EQUAL(1, menu.initializations);
	press(menu, KIT);
	CHECK_EQUAL(2, menu.initializations);
	menu.buttonAction(KIT, true, false);
	menu.buttonAction(KIT, false, false);
	CHECK_EQUAL(0, menu.actions);
	press(menu, KIT);
	CHECK_EQUAL(1, menu.actions);
	CHECK_EQUAL(2, menu.initializations);
}
TEST(SharedHorizontalMenu, MixedOccupiedSlotsMapButtonsAndShortPagesWrap) {
	MenuItem a, b, c, d, e;
	a.slots = 2;
	ExternalMenu menu({}, {&a, &b, &c, &d, &e});
	menu.beginSession(nullptr);
	menu.renderOLED();
	CHECK_EQUAL(2, menu.paging.totalPages);
	press(menu, KIT);
	CHECK_EQUAL(1, menu.actions);
	press(menu, MIDI);
	POINTERS_EQUAL(&b, menu.getCurrentItem());
	press(menu, CV);
	POINTERS_EQUAL(&c, menu.getCurrentItem());
	menu.renderOLED();
	press(menu, CROSS_SCREEN_EDIT);
	menu.renderOLED();
	POINTERS_EQUAL(&e, menu.getCurrentItem());
	CHECK_EQUAL(1, menu.paging.visiblePageNumber);
	int before = menu.initializations;
	press(menu, CV);
	POINTERS_EQUAL(&e, menu.getCurrentItem());
	CHECK_EQUAL(before, menu.initializations);
	press(menu, CROSS_SCREEN_EDIT);
	menu.renderOLED();
	CHECK_EQUAL(0, menu.paging.visiblePageNumber);
	press(menu, SCALE_MODE);
	menu.renderOLED();
	CHECK_EQUAL(1, menu.paging.visiblePageNumber);
}
TEST(SharedHorizontalMenu, ConditionalItemsUseHostRelevanceAndPreserveSelectedField) {
	MenuItem a, b, c, d, e;
	ExternalMenu menu({}, {&a, &b, &c, &d, &e});
	menu.beginSession(nullptr);
	menu.renderOLED();
	press(menu, CV);
	POINTERS_EQUAL(&d, menu.getCurrentItem());
	b.relevant = false;
	menu.renderOLED();
	POINTERS_EQUAL(&d, menu.getCurrentItem());
	CHECK_EQUAL(1, menu.paging.totalPages);
	CHECK_EQUAL(2, menu.paging.selectedItemPositionOnPage);
	d.relevant = false;
	menu.selectEncoderAction(1);
	CHECK_EQUAL(0, a.edits);
	CHECK_EQUAL(0, c.edits);
	CHECK_EQUAL(0, d.edits);
	menu.renderOLED();
	POINTERS_EQUAL(&a, menu.getCurrentItem());
}
TEST(SharedHorizontalMenu, RendererRetainsAccessibilityModesPlaceholdersAndLedCleanup) {
	MenuItem a, b;
	ExternalMenu menu({}, {&a, &b});
	menu.beginSession(nullptr);
	a.parent = &menu;
	for (auto mode :
	     {MenuHighlighting::FULL_INVERSION, MenuHighlighting::PARTIAL_INVERSION, MenuHighlighting::NO_INVERSION}) {
		FlashStorage::accessibilityMenuHighlighting = mode;
		auto& canvas = deluge::hid::display::OLED::main;
		canvas = {};
		menu.renderOLED();
		CHECK_EQUAL(mode == MenuHighlighting::FULL_INVERSION ? 1 : 0, canvas.inversions);
		CHECK_EQUAL(mode == MenuHighlighting::FULL_INVERSION ? 0 : 1, canvas.underlines);
		CHECK(canvas.pixels > 0);
		CHECK(indicator_leds::states[IndicatorLED::SYNTH]);
		CHECK_FALSE(indicator_leds::states[IndicatorLED::KIT]);
	}
	screen.notification = true;
	menu.endSession();
	CHECK_FALSE(screen.notification);
	for (auto led : {IndicatorLED::SYNTH, IndicatorLED::KIT, IndicatorLED::MIDI, IndicatorLED::CV,
	                 IndicatorLED::SCALE_MODE, IndicatorLED::CROSS_SCREEN_EDIT})
		CHECK_FALSE(indicator_leds::states[led]);
	POINTERS_EQUAL(nullptr, a.parent);
	POINTERS_EQUAL(nullptr, b.parent);
}
TEST(SharedHorizontalMenu, HorizontalDecimalClampsWideOffsetsAndUsesHeldSelectFineStep) {
	Decimal decimal;
	ExternalMenu menu({}, {&decimal});
	menu.beginSession(nullptr);
	menu.selectEncoderAction(2000000000);
	CHECK_EQUAL(2450, decimal.written);
	menu.selectEncoderAction(-2000000000);
	CHECK_EQUAL(-2450, decimal.written);
	decimal.value = -123;
	menu.selectEncoderAction(2);
	CHECK_EQUAL(77, decimal.written);
	Buttons::selectPressed = true;
	menu.selectEncoderAction(-1);
	CHECK_EQUAL(76, decimal.written);
	CHECK_TRUE(Buttons::selectButtonPressUsedUp);
	decimal.renderInHorizontalMenu({0, 14, 32, 25});
	STRCMP_EQUAL("0.76", deluge::hid::display::OLED::main.lines.back().c_str());
}
TEST(SharedHorizontalMenu, VerticalDecimalRetainsCursorScrolling) {
	Decimal decimal;
	soundEditor.numberEditPos = 7;
	soundEditor.numberEditSize = 1000;
	soundEditor.numberScrollAmount = 3;
	decimal.selectEncoderAction(1);
	CHECK_EQUAL(1123, decimal.written);
	CHECK_EQUAL(4, soundEditor.numberScrollAmount);
}
TEST(SharedHorizontalMenu, DefaultHostSampleDecimalRetainsRangeAndCoarseFineEditing) {
	Decimal decimal;
	decimal.range = true;
	HorizontalMenu menu({}, {&decimal});
	MultiRange range;
	soundEditor.currentMultiRange = &range;
	soundEditor.currentMultiRangeIndex = 3;
	menu.beginSession(&decimal);
	menu.renderOLED();
	menu.selectEncoderAction(1);
	CHECK_EQUAL(223, decimal.written);
	Buttons::shiftPressed = true;
	probeMenuTime += 1;
	menu.selectEncoderAction(1);
	CHECK_EQUAL(224, decimal.written);
	POINTERS_EQUAL(&range, soundEditor.currentMultiRange);
	CHECK_EQUAL(3, soundEditor.currentMultiRangeIndex);
}
TEST(SharedHorizontalMenu, EmptyAndAllIrrelevantMenusHaveNoSelectionOrActions) {
	MenuItem hidden;
	hidden.relevant = false;
	ExternalMenu empty({}, {}), irrelevant({}, {&hidden});
	for (auto* menu : {&empty, &irrelevant}) {
		menu->beginSession(nullptr);
		menu->renderOLED();
		POINTERS_EQUAL(nullptr, menu->getCurrentItem());
		menu->selectEncoderAction(1);
		press(*menu, SYNTH);
		press(*menu, CROSS_SCREEN_EDIT);
		CHECK_EQUAL(0, menu->actions);
		CHECK_EQUAL(0, menu->initializations);
		menu->endSession();
	}
}
TEST(SharedHorizontalMenu, FixedDisabledItemsHaveNoSelectionLed) {
	MenuItem hidden;
	hidden.relevant = false;
	ExternalMenu menu({}, {&hidden}, HorizontalMenu::FIXED);
	menu.beginSession(nullptr);
	menu.renderOLED();
	POINTERS_EQUAL(nullptr, menu.getCurrentItem());
	CHECK_FALSE(indicator_leds::states[IndicatorLED::SYNTH]);
	press(menu, SYNTH);
	CHECK_EQUAL(0, menu.actions);
	CHECK_EQUAL(0, menu.initializations);
}
