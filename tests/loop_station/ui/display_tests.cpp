#include "fixture.h"

using namespace loop_station;
using namespace deluge::gui::menu_item;
using namespace deluge::hid::button;
using deluge::gui::menu_item::loop_station::loopStationSettingsMenu;
using deluge::hid::display::OLED;

// clang-format off
TEST_GROUP(LoopSettingsDisplay) {
	void teardown() override {
		deluge::l10n::probeTranslations.clear();
		FlashStorage::accessibilityMenuHighlighting = MenuHighlighting::FULL_INVERSION;
	}
};
// clang-format on

TEST(LoopSettingsDisplay, CompactLabelsKeepFullNotificationNames) {
	loop_ui_test::Fixture f;
	auto& menu = loopStationSettingsMenu;
	CHECK(menu.openTrack(0));
	auto* transpose = menu.items_.item(MenuField::Transpose);
	StringBuf label;
	transpose->getColumnLabel(label);
	STRCMP_EQUAL("Pitch", label.c_str());
	STRCMP_EQUAL("Transpose", transpose->getName());
	menu.horizontalMenu_.selectField(MenuField::Transpose);
	menu.selectEncoderAction(1);
	STRCMP_EQUAL("Transpose", f.screen.notificationName.c_str());
	STRCMP_EQUAL("1.00", f.screen.notificationValue.c_str());
}

static bool hasText(const deluge::hid::display::oled_canvas::Canvas& canvas, const char* text) {
	return std::ranges::find(canvas.lines, text) != canvas.lines.end();
}

static bool hasTextAt(const char* text, int x, int y) {
	const auto& canvas = OLED::main;
	for (size_t i = 0; i < canvas.lines.size(); ++i)
		if (canvas.lines[i] == text && canvas.textX[i] == x && canvas.textY[i] == y)
			return true;
	return false;
}

static void checkCanvas() {
	auto& canvas = OLED::main;
	for (auto byte : canvas.leadingCanary)
		CHECK_EQUAL(0xa5, byte);
	for (auto byte : canvas.trailingCanary)
		CHECK_EQUAL(0xa5, byte);
	CHECK(std::ranges::any_of(std::span(&canvas.image_[0][0], sizeof(canvas.image_)), [](auto b) { return b != 0; }));
	for (size_t i = 0; i < canvas.lines.size(); ++i) {
		CHECK(canvas.textY[i] >= 0 && canvas.textY[i] + canvas.textHeight[i] <= OLED_MAIN_HEIGHT_PIXELS);
		CHECK_TEXT(canvas.textX[i] >= 0 && canvas.textX[i] + canvas.textWidth[i] <= OLED_MAIN_WIDTH_PIXELS,
		           (canvas.lines[i] + " x=" + std::to_string(canvas.textX[i])
		            + " width=" + std::to_string(canvas.textWidth[i]))
		               .c_str());
	}
}

TEST(LoopSettingsDisplay, SharedSettingsColumnsStayWithinOled) {
	loop_ui_test::Fixture f;
	auto& menu = loopStationSettingsMenu;
	auto& canvas = OLED::main;
	canvas.leadingCanary.fill(0xa5);
	canvas.trailingCanary.fill(0xa5);
	for (bool localized : {false, true}) {
		if (localized) {
			using deluge::l10n::String;
			for (auto id :
			     {String::STRING_FOR_LOOP_STATION_RECORD_SHORT, String::STRING_FOR_LOOP_STATION_START_SHORT,
			      String::STRING_FOR_LOOP_STATION_STOP_SHORT, String::STRING_FOR_LOOP_STATION_INTERVAL_SHORT,
			      String::STRING_FOR_LOOP_STATION_SOURCE_SHORT, String::STRING_FOR_LOOP_STATION_MONITOR_SHORT,
			      String::STRING_FOR_LOOP_STATION_DURATION_SHORT, String::STRING_FOR_LOOP_STATION_PLAYBACK_SHORT,
			      String::STRING_FOR_LOOP_STATION_TRANSPOSE_SHORT, String::STRING_FOR_LOOP_STATION_COLOUR_SHORT,
			      String::STRING_FOR_LOOP_STATION_COPY_SHORT})
				deluge::l10n::probeTranslations[id] = "Lange lokalisierte Beschriftung";
		}
		for (int cents : {-2450, 2450}) {
			for (auto highlight : {MenuHighlighting::FULL_INVERSION, MenuHighlighting::PARTIAL_INVERSION,
			                       MenuHighlighting::NO_INVERSION}) {
				FlashStorage::accessibilityMenuHighlighting = highlight;
				for (bool start : {false, true})
					for (bool stop : {false, true}) {
						auto settings = f.model->engine().settings();
						settings.startAction.mode = start ? ActionMode::Synced : ActionMode::Immediate;
						settings.stopAction.mode = stop ? ActionMode::Synced : ActionMode::Immediate;
						settings.tracks[0].transposeCents = cents;
						CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
						for (auto scope : {MenuScope::Global, MenuScope::Track}) {
							CHECK(scope == MenuScope::Global ? menu.openGlobal() : menu.openTrack(0));
							int index = 0;
							for (auto field : menu.fields_.rows | std::views::take(menu.fields_.count)) {
								menu.horizontalMenu_.selectField(field);
								const auto inversions = canvas.inversions, underlines = canvas.underlines;
								menu.renderOLED(canvas);
								checkCanvas();
								CHECK(hasText(canvas, scope == MenuScope::Global ? "Loop Global" : "Loop Track"));
								CHECK_EQUAL(highlight == MenuHighlighting::FULL_INVERSION ? 1 : 0,
								            canvas.inversions - inversions);
								CHECK_EQUAL(highlight == MenuHighlighting::FULL_INVERSION ? 0 : 1,
								            canvas.underlines - underlines);
								CHECK(field == menu.horizontalMenu_.selectedField());
								const bool twoPages = menu.fields_.count > 4;
								if (twoPages) {
									CHECK(hasTextAt("2", 121, 6));
									CHECK(hasTextAt(index < 4 ? "1" : "2", 110, 6));
								}
								const std::array leds{IndicatorLED::SYNTH, IndicatorLED::KIT, IndicatorLED::MIDI,
								                      IndicatorLED::CV};
								for (int i = 0; i < 4; ++i)
									CHECK_EQUAL(i == index % 4, indicator_leds::states[leds[i]]);
								if (field == MenuField::Transpose)
									CHECK(hasText(canvas, cents < 0 ? "-24.50" : "24.50"));
								const auto pageItems = std::min<int>(4, menu.fields_.count - index / 4 * 4);
								for (int column = pageItems; column < 4; ++column) {
									deluge::hid::display::oled_canvas::Canvas placeholder;
									const int left = column * 32;
									for (int x : {left + 8, left + 13, left + 18, left + 23}) {
										placeholder.drawPixel(x, 20);
										placeholder.drawPixel(x, 41);
									}
									for (int y : {23, 28, 33, 38}) {
										placeholder.drawPixel(left + 5, y);
										placeholder.drawPixel(left + 26, y);
									}
									for (int y = 19; y < 44; ++y)
										for (int x = left; x < left + 32; ++x)
											CHECK_EQUAL(placeholder.image_[y / 8][x] & (1 << (y % 8)),
											            canvas.image_[y / 8][x] & (1 << (y % 8)));
								}
								++index;
							}
							menu.closeMenu();
						}
					}
			}
		}
	}
	FlashStorage::accessibilityMenuHighlighting = MenuHighlighting::FULL_INVERSION;
}

TEST(LoopSettingsDisplay, CopyReviewFitsOled) {
	loop_ui_test::Fixture f;
	auto& menu = loopStationSettingsMenu;
	auto& canvas = OLED::main;
	canvas.leadingCanary.fill(0xa5);
	canvas.trailingCanary.fill(0xa5);
	for (bool localized : {false, true}) {
		using deluge::l10n::String;
		if (localized) {
			deluge::l10n::probeTranslations[String::STRING_FOR_LOOP_STATION_COPY_FROM] = "Von Spur %u";
			deluge::l10n::probeTranslations[String::STRING_FOR_LOOP_STATION_COPY_TARGET] = "Zu Spur %u";
			deluge::l10n::probeTranslations[String::STRING_FOR_LOOP_STATION_COPY_CONFIRM] =
			    "SELECT kopiert BACK abbricht";
		}
		for (SlotId source : {SlotId{0}, SlotId{7}}) {
			CHECK(menu.openTrack(source));
			const SlotId target = source == 0 ? 7 : 0;
			menu.copyTarget_ = target;
			menu.requestCopyReview();
			menu.copyTarget_ = source; // Review must retain its frozen target.
			menu.renderOLED(canvas);
			checkCanvas();
			char text[32];
			std::snprintf(text, sizeof(text), localized ? "Von Spur %u" : "From Track %u", source + 1);
			CHECK(hasText(canvas, text));
			std::snprintf(text, sizeof(text), localized ? "Zu Spur %u" : "To Track %u", target + 1);
			CHECK(hasText(canvas, text));
			CHECK(hasText(canvas, localized ? "SELECT kopiert BACK abbricht" : "SELECT: copy BACK: cancel"));
			menu.closeMenu();
		}
	}
}

TEST(LoopSettingsDisplay, FeatureOffAndSevenSegmentKeepListEditing) {
	for (bool seven : {false, true}) {
		loop_ui_test::Fixture f(false);
		f.screen.oled = !seven;
		auto& menu = loopStationSettingsMenu;
		CHECK(menu.openTrack(0));
		menu.selectEncoderAction(4);
		CHECK(menu.fields_.rows[menu.row_] == MenuField::Transpose);
		menu.buttonAction(SELECT_ENC, true, false);
		menu.buttonAction(SELECT_ENC, false, false);
		CHECK(menu.editing_);
		menu.selectEncoderAction(1);
		CHECK_EQUAL(100, f.model->engine().settings().tracks[0].transposeCents);
		Buttons::shiftPressed = true;
		menu.selectEncoderAction(1);
		Buttons::shiftPressed = false;
		CHECK_EQUAL(101, f.model->engine().settings().tracks[0].transposeCents);
		if (seven)
			STRCMP_EQUAL("+1.01", f.screen.popups.back().c_str());
		menu.buttonAction(SELECT_ENC, true, false);
		menu.buttonAction(SELECT_ENC, false, false);
		CHECK_FALSE(menu.editing_);
		menu.selectEncoderAction(2);
		CHECK(menu.fields_.rows[menu.row_] == MenuField::CopyToTrack);
		auto press = [&] {
			menu.buttonAction(SELECT_ENC, true, false);
			menu.buttonAction(SELECT_ENC, false, false);
		};
		press();
		CHECK(menu.choosingCopyTarget_);
		menu.selectEncoderAction(1);
		const auto target = menu.copyTarget_;
		press();
		CHECK(menu.copyReview_);
		menu.buttonAction(BACK, true, false);
		CHECK_FALSE(menu.copyReview_);
		CHECK(getCurrentUI() == &menu);
		CHECK_EQUAL(0, f.model->engine().settings().tracks[target].transposeCents);
		press();
		press();
		CHECK(menu.copyReview_);
		press();
		CHECK_FALSE(menu.copyReview_);
		CHECK_EQUAL(101, f.model->engine().settings().tracks[target].transposeCents);
		menu.buttonAction(BACK, true, false);
		CHECK_EQUAL(101, f.model->engine().settings().tracks[0].transposeCents);
	}
}

TEST(LoopSettingsDisplay, ReturningFromFallbackDoesNotChangePreference) {
	for (bool preference : {false, true}) {
		for (bool oled : {false, true}) {
			if (preference && oled)
				continue; // This combination is the horizontal host, not a fallback.
			loop_ui_test::Fixture f(preference);
			f.screen.oled = oled;
			auto& menu = loopStationSettingsMenu;
			CHECK(menu.openTrack(0));
			menu.buttonAction(BACK, true, false);
			CHECK_EQUAL(preference, runtimeFeatureSettings.horizontal);
			f.screen.oled = true;
			HorizontalMenu ordinary{{}, {}};
			CHECK_EQUAL(preference ? Submenu::HORIZONTAL : Submenu::VERTICAL, ordinary.renderingStyle());
			MenuItem item;
			HorizontalMenu populated({}, {&item});
			populated.beginSession(nullptr);
			CHECK(item.parent == &populated);
			populated.endSession();
			CHECK(item.parent == nullptr);
			CHECK_EQUAL(preference, runtimeFeatureSettings.horizontal);
		}
	}
}
