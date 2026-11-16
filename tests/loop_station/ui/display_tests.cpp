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

namespace {
using ::loop_station::HistoryNavigationState;
using ::loop_station::HistoryTarget;
using ::loop_station::RestoreId;
using ::loop_station::Status;

HistoryNavigationResult driveHistory(loop_ui_test::Fixture& f, RestoreId id) {
	for (unsigned i = 0; i < 5000; ++i) {
		f.model->serviceHistoryNavigation(128);
		const auto result = f.model->historyNavigationResult();
		if (result.id.generation == id.generation && !result.pending())
			return result;
	}
	return f.model->historyNavigationResult();
}

// Draw the real production Loop Station OLED into a checked, canaried canvas.
void renderLoopOled(loop_ui_test::Fixture& f) {
	auto& canvas = OLED::main;
	canvas.leadingCanary.fill(0xa5);
	canvas.trailingCanary.fill(0xa5);
	f.view.renderOLED(canvas);
	checkCanvas();
}

// Record a short phrase on an arbitrary slot through the real model path.
void recordSlot(loop_ui_test::Fixture& f, SlotId slot) {
	CHECK_EQUAL(int(Status::Ok), int(f.model->ensureBound(slot)));
	CHECK_EQUAL(int(Status::Ok), int(f.model->dispatch({CommandType::Press, slot})));
	CHECK(f.model->engine().state(slot).capture);
	f.renderAudio();
	CHECK_EQUAL(int(Status::Ok), int(f.model->dispatch({CommandType::Press, slot})));
	CHECK(f.model->engine().state(slot).hasPhrase);
}

void overdubLayer(loop_ui_test::Fixture& f, SlotId slot) {
	// Leave any previous double-tap window behind before opening a fresh gate.
	f.host.time += GestureRollback::kProvisionalSamples;
	CHECK_EQUAL(int(Status::Ok), int(f.model->dispatch({CommandType::Press, slot})));
	CHECK(f.model->engine().state(slot).capture);
	f.renderAudio(1);
	// Pass the double-tap cancellation window: this is a confirmed overdub close.
	f.host.time += GestureRollback::kProvisionalSamples;
	CHECK_EQUAL(int(Status::Ok), int(f.model->dispatch({CommandType::Press, slot})));
	CHECK_FALSE(f.model->engine().state(slot).capture);
}

Status undoSlot(loop_ui_test::Fixture& f, SlotId slot) {
	HistoryTarget target{};
	const auto status = f.model->historyTarget(slot, false, target);
	if (status != Status::Ok)
		return status;
	RestoreId id{};
	const auto requested = f.model->requestHistoryNavigation(target, id);
	if (requested != Status::Ok)
		return requested;
	return driveHistory(f, id).state == HistoryNavigationState::Complete ? Status::Ok : Status::SourceUnavailable;
}

// Use real retained versions on every other slot to exhaust the fixed detached
// records. Stops at the first operation that can no longer be accepted.
void saturateDetachedRecords(loop_ui_test::Fixture& f) {
	for (SlotId slot = 1; slot < kTrackCount; ++slot) {
		recordSlot(f, slot);
		for (unsigned layer = 0; layer < 3; ++layer)
			overdubLayer(f, slot);
		for (unsigned undo = 0; undo < 4; ++undo)
			if (undoSlot(f, slot) != Status::Ok)
				return;
	}
}
} // namespace

TEST(LoopSettingsDisplay, ConfirmationSurvivesGraphicsRefreshOnOledAndSevenSegment) {
	// OLED: the prompt is drawn from view state on every refresh.
	{
		loop_ui_test::Fixture f;
		f.recordPhrase();
		f.view.padAction(0, 7, 100);
		CHECK_EQUAL(0, f.model->engine().selected());
		CHECK_TRUE(f.model->canUndo(0));
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
		for (int refresh = 0; refresh < 2; ++refresh) {
			renderLoopOled(f);
			CHECK(hasText(OLED::main, "TRACK 1"));
			CHECK(hasText(OLED::main, "UNDO TRACK 1?"));
			CHECK(hasText(OLED::main, "BACK TO CONFIRM"));
		}
	}
	// Seven segment: the view owns a persistent scrolling layer, pushes it only
	// when the content changes, and removes it on cancel.
	{
		loop_ui_test::Fixture f;
		f.screen.oled = false;
		f.screen.setText("LOOP"); // the resting text opened() installs on a real 7SEG
		f.recordPhrase();
		f.view.padAction(0, 7, 100);
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(f.screen.scrollingTexts.size() == 1);
		CHECK_FALSE(f.screen.scrollingTexts.empty());
		if (!f.screen.scrollingTexts.empty())
			STRCMP_EQUAL("UNDO TRACK 1? BACK TO CONFIRM", f.screen.scrollingTexts.back().c_str());
		// A periodic refresh must not re-push (which would reset scrolling/blink).
		f.view.graphicsRoutine();
		CHECK_EQUAL(1, f.screen.scrollingTexts.size());
		// A long sample-clock advance with no further action does not expire it.
		f.host.time += 10ull * ::loop_station::kSampleRate;
		probeSampleClock = f.host.time;
		f.view.graphicsRoutine();
		CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
		CHECK_EQUAL(1, f.screen.scrollingTexts.size());
		// An unrelated action cancels and removes the owned layer, restoring the
		// view's resting seven-segment text without a new scrolling push.
		f.view.padAction(4, 7, 100);
		CHECK(LoopStationView::HistoryPromptState::None == f.view.historyPromptState_);
		STRCMP_EQUAL("LOOP", f.screen.text.c_str());
		CHECK_EQUAL(1, f.screen.scrollingTexts.size());
	}
	// The shifted prompt shows the Redo action and its distinct instruction.
	{
		loop_ui_test::Fixture f;
		f.recordPhrase();
		f.view.padAction(0, 7, 100);
		// Undo to empty first so Redo is eligible.
		f.view.buttonAction(BACK, true, false);
		f.view.buttonAction(BACK, false, false);
		f.view.buttonAction(BACK, true, false);
		f.view.buttonAction(BACK, false, false);
		CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, f.view.historyId_).state));
		f.view.graphicsRoutine();
		// Arm Redo through the real shared SHIFT dispatch.
		::view.reset();
		::view.result = ActionResult::NOT_DEALT_WITH;
		CHECK(f.sharedShift(f.view, true) == ActionResult::NOT_DEALT_WITH);
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
		CHECK_TRUE(f.view.historyTarget_.redo);
		renderLoopOled(f);
		CHECK(hasText(OLED::main, "TRACK 1"));
		CHECK(hasText(OLED::main, "REDO TRACK 1?"));
		CHECK(hasText(OLED::main, "SHIFT+BACK TO CONFIRM"));
	}
}

TEST(LoopSettingsDisplay, ApplyingAndTerminalResultReflectActualModelStatus) {
	// No completion message before the model commits.
	{
		loop_ui_test::Fixture f;
		f.recordPhrase();
		f.view.padAction(0, 7, 100);
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(LoopStationView::HistoryPromptState::Applying == f.view.historyPromptState_);
		renderLoopOled(f);
		CHECK(hasText(OLED::main, "UNDOING"));
		CHECK_FALSE(hasText(OLED::main, "UNDONE"));
		// The real foreground owner drives the accepted job to its terminal result.
		const auto id = f.view.historyId_;
		CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, id).state));
		f.view.graphicsRoutine(); // observe the result from the regular refresh
		renderLoopOled(f);
		CHECK(hasText(OLED::main, "UNDONE"));
	}
	// Busy is reported when the single restore gate is already owned.
	{
		loop_ui_test::Fixture f;
		f.screen.oled = false;
		f.recordPhrase();
		f.view.padAction(0, 7, 100);
		HistoryTarget target{};
		RestoreId busyId{};
		CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
		CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, busyId)));
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(LoopStationView::HistoryPromptState::Result == f.view.historyPromptState_);
		CHECK_EQUAL(int(Status::Busy), int(f.view.historyStatus_));
		CHECK_FALSE(f.screen.scrollingTexts.empty());
		if (!f.screen.scrollingTexts.empty())
			STRCMP_EQUAL("Track 1 BUSY", f.screen.scrollingTexts.back().c_str());
	}
	// An empty track honestly reports no history instead of arming.
	{
		loop_ui_test::Fixture f;
		f.screen.oled = false;
		f.view.padAction(0, 7, 100);
		CHECK_FALSE(f.model->engine().state(0).hasPhrase);
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(LoopStationView::HistoryPromptState::Result == f.view.historyPromptState_);
		CHECK_EQUAL(int(Status::SourceUnavailable), int(f.view.historyStatus_));
		CHECK_FALSE(f.screen.scrollingTexts.empty());
		if (!f.screen.scrollingTexts.empty())
			STRCMP_EQUAL("Track 1 NO UNDO", f.screen.scrollingTexts.back().c_str());
	}
}

TEST(LoopSettingsDisplay, BackLedFollowsSelectedOperationAvailability) {
	loop_ui_test::Fixture f;
	f.view.updateLed();
	CHECK_FALSE(indicator_leds::states[IndicatorLED::BACK]); // no track selected
	CHECK_TRUE(indicator_leds::states[IndicatorLED::CROSS_SCREEN_EDIT]);

	f.view.padAction(0, 7, 100); // bind/select an empty track
	CHECK_EQUAL(0, f.model->engine().selected());
	f.view.updateLed();
	CHECK_FALSE(indicator_leds::states[IndicatorLED::BACK]); // no history to undo

	f.recordPhrase();
	f.view.updateLed();
	CHECK_TRUE(indicator_leds::states[IndicatorLED::BACK]); // Undo available
	CHECK_TRUE(indicator_leds::states[IndicatorLED::CROSS_SCREEN_EDIT]);

	// SHIFT follows the eligible operation: Redo is not available yet.
	::view.reset();
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.sharedShift(f.view, true) == ActionResult::NOT_DEALT_WITH);
	f.view.updateLed();
	CHECK_FALSE(indicator_leds::states[IndicatorLED::BACK]);

	// Undo the only layer, then Redo becomes the eligible shifted operation.
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.sharedShift(f.view, false) == ActionResult::NOT_DEALT_WITH);
	f.view.buttonAction(BACK, true, false);
	f.view.buttonAction(BACK, false, false);
	f.view.buttonAction(BACK, true, false);
	f.view.buttonAction(BACK, false, false);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, f.view.historyId_).state));
	f.view.graphicsRoutine();
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	f.view.updateLed();
	CHECK_FALSE(indicator_leds::states[IndicatorLED::BACK]); // no Undo
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.sharedShift(f.view, true) == ActionResult::NOT_DEALT_WITH);
	f.view.updateLed();
	CHECK_TRUE(indicator_leds::states[IndicatorLED::BACK]); // Redo available
	CHECK_TRUE(indicator_leds::states[IndicatorLED::CROSS_SCREEN_EDIT]);
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.sharedShift(f.view, false) == ActionResult::NOT_DEALT_WITH);
}

TEST(LoopSettingsDisplay, ReturnFromMenuCannotReviveOldConfirmation) {
	loop_ui_test::Fixture f;
	f.screen.oled = false; // also exercise the owned seven-segment layer
	f.recordPhrase();
	f.view.padAction(0, 7, 100);
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
	CHECK_TRUE(f.view.historyFeedbackOwned_);
	CHECK_EQUAL(1, f.screen.scrollingTexts.size());

	// Opening the shared menu drops the prompt and its owned layer as an outgoing
	// action, restoring the view's resting seven-segment text.
	CHECK(f.view.buttonAction(SELECT_ENC, true, false) == ActionResult::DEALT_WITH);
	POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
	CHECK(LoopStationView::HistoryPromptState::None == f.view.historyPromptState_);
	CHECK_FALSE(f.view.historyFeedbackOwned_);
	CHECK_EQUAL(1, f.screen.scrollingTexts.size());
	// The menu keeps BACK; focus regain must not resurrect the old confirmation.
	loopStationSettingsMenu.buttonAction(BACK, true, false);
	POINTERS_EQUAL(&f.view, getCurrentUI());
	CHECK(LoopStationView::HistoryPromptState::None == f.view.historyPromptState_);
	CHECK_FALSE(f.view.historyFeedbackOwned_);

	// A single fresh press only arms; it can never stand in for the old second press.
	f.view.buttonAction(BACK, false, false);
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
	CHECK_TRUE(f.view.historyFeedbackOwned_);
	CHECK_EQUAL(2, f.screen.scrollingTexts.size());
	CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
	CHECK_TRUE(f.model->engine().state(0).hasPhrase);
}

TEST(LoopSettingsDisplay, FailedSevenSegmentPromptInstallCannotConfirmAndRetriesAfterRecovery) {
	loop_ui_test::Fixture f;
	f.screen.oled = false;
	f.screen.setText("LOOP"); // the resting text opened() installs on a real 7SEG
	f.recordPhrase();
	f.view.padAction(0, 7, 100);
	f.screen.failScrollingAllocation = true;
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
	CHECK_FALSE(f.view.historyFeedbackOwned_);
	CHECK_FALSE(f.screen.overlayVisible());
	// Failed feedback must not authorize the second released BACK.
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
	CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
	CHECK(f.model->engine().state(0).hasPhrase);

	f.screen.failScrollingAllocation = false;
	f.view.graphicsRoutine();
	CHECK(f.view.historyFeedbackOwned_);
	CHECK(f.screen.overlayVisible());
	POINTERS_EQUAL(f.screen.currentScrollingLayer, f.view.historyFeedbackLayer_);
	const auto installs = f.screen.scrollingTexts.size();
	f.view.graphicsRoutine();
	CHECK_EQUAL(installs, f.screen.scrollingTexts.size()); // Refresh does not restart scrolling.
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Applying == f.view.historyPromptState_);
}

TEST(LoopSettingsDisplay, AllocatorYieldDuringInstallDoesNotAdoptStaleFeedback) {
	loop_ui_test::Fixture f;
	f.screen.oled = false;
	f.screen.setText("LOOP"); // the resting text opened() installs on a real 7SEG
	f.recordPhrase();
	f.view.padAction(0, 7, 100);
	bool first = true;
	f.screen.onScrollingAllocation = [&] {
		if (first) {
			first = false;
			// The production allocator may yield and re-enter while installing.
			f.view.cancelHistoryConfirmation();
		}
	};
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	// The prompt died mid-allocation; the freshly installed layer must not be
	// adopted as visible stale feedback.
	CHECK(LoopStationView::HistoryPromptState::None == f.view.historyPromptState_);
	CHECK_FALSE(f.view.historyFeedbackOwned_);
	CHECK(f.view.historyFeedbackLayer_ == nullptr);
	CHECK_FALSE(f.screen.overlayVisible());
	CHECK_EQUAL(0u, f.screen.overlayStack.size());
	STRCMP_EQUAL("LOOP", f.screen.text.c_str());
}

TEST(LoopSettingsDisplay, DestructivePressRequiresVisibleFeedback) {
	loop_ui_test::Fixture f;
	f.screen.oled = false;
	f.recordPhrase();
	f.view.padAction(0, 7, 100);
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(f.screen.overlayVisible());
	// Installed but hidden behind another display owner: never submit.
	f.screen.popupActive = true;
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
	CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
	CHECK(f.model->engine().state(0).hasPhrase);
	f.screen.popupActive = false;
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Applying == f.view.historyPromptState_);
}

TEST(LoopSettingsDisplay, PromptCleanupRemovesOnlyOwnedOverlay) {
	loop_ui_test::Fixture f;
	f.screen.oled = false;
	f.screen.setText("LOOP"); // the resting text opened() installs on a real 7SEG
	f.recordPhrase();
	f.view.padAction(0, 7, 100);
	f.view.buttonAction(BACK, true, false);
	f.view.buttonAction(BACK, false, false);
	CHECK(f.screen.overlayVisible());
	void* unrelated = f.screen.installScrollingOverlay("MENU");
	CHECK(unrelated != nullptr);
	POINTERS_EQUAL(unrelated, f.screen.currentScrollingLayer);
	// A failing allocator must not block allocation-free owned-layer cleanup, and
	// the newer unrelated layer must stay on top.
	f.screen.failScrollingAllocation = true;
	f.view.cancelHistoryConfirmation();
	CHECK_FALSE(f.view.historyFeedbackOwned_);
	CHECK_EQUAL(1u, f.screen.removeAttempts);
	POINTERS_EQUAL(unrelated, f.screen.currentScrollingLayer);
	CHECK(f.screen.overlayVisible());
	STRCMP_EQUAL("LOOP", f.screen.text.c_str());
	f.screen.failScrollingAllocation = false;
}

TEST(LoopSettingsDisplay, HistoryPromptOverlayCleanupPreservesBaseAndPopupOwner) {
	loop_ui_test::Fixture f;
	f.screen.oled = false;
	f.screen.setText("LOOP"); // the resting text opened() installs on a real 7SEG
	f.recordPhrase();
	f.view.padAction(0, 7, 100);
	f.view.buttonAction(BACK, true, false);
	f.view.buttonAction(BACK, false, false);
	CHECK(f.screen.overlayVisible());
	const auto base = f.screen.text;
	f.screen.popupActive = true;
	f.view.cancelHistoryConfirmation();
	CHECK_FALSE(f.screen.overlayVisible());
	CHECK(f.screen.popupActive);
	STRCMP_EQUAL(base.c_str(), f.screen.text.c_str());
	CHECK_FALSE(f.view.historyFeedbackOwned_);
}

// M5: a real model rejection under actual detached-record pressure must reach the
// view through released BACK edges, not a test-assigned status. Capture stays open
// and both display paths report the honest failure without a false completion.
TEST(LoopSettingsDisplay, RealDetachedPressureReportsNoMemoryAndPreservesCapture) {
	// Seven-segment path.
	{
		loop_ui_test::Fixture f;
		f.screen.oled = false;
		f.screen.setText("LOOP");
		f.recordPhrase();
		f.view.padAction(0, 7, 100);
		CHECK_EQUAL(0, f.model->engine().selected());
		saturateDetachedRecords(f);
		CHECK_EQUAL(int(Status::Ok), int(f.model->dispatch({CommandType::Press, 0})));
		CHECK(f.model->engine().state(0).capture);
		f.renderAudio(1);
		HistoryTarget target{};
		CHECK_EQUAL(int(Status::NoMemory), int(f.model->historyTarget(0, false, target)));
		const auto prior = f.model->historyNavigationResult();
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
		CHECK(LoopStationView::HistoryPromptState::Result == f.view.historyPromptState_);
		CHECK_EQUAL(int(Status::NoMemory), int(f.view.historyStatus_));
		CHECK_FALSE(f.screen.scrollingTexts.empty());
		if (!f.screen.scrollingTexts.empty())
			STRCMP_EQUAL("Track 1 NO MEMORY", f.screen.scrollingTexts.back().c_str());
		CHECK(f.model->engine().state(0).capture);
		CHECK(f.model->engine().state(0).playing);
		CHECK(f.model->engine().state(0).hasPhrase);
		CHECK_EQUAL(prior.id.generation, f.model->historyNavigationResult().id.generation);
	}
	// OLED path.
	{
		loop_ui_test::Fixture f;
		f.recordPhrase();
		f.view.padAction(0, 7, 100);
		CHECK_EQUAL(0, f.model->engine().selected());
		saturateDetachedRecords(f);
		CHECK_EQUAL(int(Status::Ok), int(f.model->dispatch({CommandType::Press, 0})));
		f.renderAudio(1);
		HistoryTarget target{};
		CHECK_EQUAL(int(Status::NoMemory), int(f.model->historyTarget(0, false, target)));
		const auto prior = f.model->historyNavigationResult();
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
		renderLoopOled(f);
		CHECK(hasText(OLED::main, "TRACK 1"));
		CHECK(hasText(OLED::main, "NO MEMORY"));
		CHECK_FALSE(hasText(OLED::main, "UNDONE"));
		CHECK(f.model->engine().state(0).capture);
		CHECK_EQUAL(prior.id.generation, f.model->historyNavigationResult().id.generation);
	}
}

// M7: while confirmation identity remains current, eligibility is rechecked on
// refresh. Incoming frames, SHIFT and a release keep it; real pressure replaces
// the stale instruction with an honest reason and never submits the operation.
TEST(LoopSettingsDisplay, EligibilityLossReplacesCurrentConfirmationWithHonestReason) {
	loop_ui_test::Fixture f;
	f.screen.oled = false;
	f.screen.setText("LOOP");
	f.recordPhrase();
	CHECK_EQUAL(int(Status::Ok), int(f.model->dispatch({CommandType::Press, 0})));
	CHECK(f.model->engine().state(0).capture);
	f.renderAudio(1);
	f.view.padAction(0, 7, 100);
	CHECK_EQUAL(0, f.model->engine().selected());
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
	// Incoming capture frames are not an identity change.
	f.renderAudio(2);
	f.view.graphicsRoutine();
	CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
	// SHIFT and a release edge do not cancel the confirmation either.
	::view.reset();
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.sharedShift(f.view, true) == ActionResult::NOT_DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.sharedShift(f.view, false) == ActionResult::NOT_DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Confirm == f.view.historyPromptState_);
	// Real detached-resource pressure while the target identity stays current.
	saturateDetachedRecords(f);
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::NoMemory), int(f.model->historyTarget(0, false, target)));
	const auto prior = f.model->historyNavigationResult();
	f.view.graphicsRoutine();
	CHECK(LoopStationView::HistoryPromptState::Result == f.view.historyPromptState_);
	CHECK_EQUAL(int(Status::NoMemory), int(f.view.historyStatus_));
	CHECK_FALSE(f.screen.scrollingTexts.empty());
	if (!f.screen.scrollingTexts.empty())
		STRCMP_EQUAL("Track 1 NO MEMORY", f.screen.scrollingTexts.back().c_str());
	CHECK(f.model->engine().state(0).capture);
	CHECK_EQUAL(prior.id.generation, f.model->historyNavigationResult().id.generation);
	// A resulting BACK press arms a fresh target honestly; it cannot submit.
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(LoopStationView::HistoryPromptState::Result == f.view.historyPromptState_);
	CHECK_EQUAL(prior.id.generation, f.model->historyNavigationResult().id.generation);
}
