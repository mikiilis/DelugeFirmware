#include "fixture.h"
#include "shared_probe_support.h"

using namespace loop_station;
using namespace deluge::hid::button;
using deluge::gui::menu_item::loop_station::loopStationSettingsMenu;
using loop_ui_test::Fixture;

namespace {
constexpr int centres[8][2] = {{1, 6}, {5, 6}, {9, 6}, {13, 6}, {1, 2}, {5, 2}, {9, 2}, {13, 2}};
void press(deluge::hid::Button b) {
	probeMenuTime += 1; // Reset shared navigation debounce between physical fixture events.
	loopStationSettingsMenu.buttonAction(b, true, false);
}
void release() {
	loopStationSettingsMenu.buttonAction(SELECT_ENC, false, false);
}
void turn(int8_t n) {
	loopStationSettingsMenu.selectEncoderAction(n);
}
void copyColumn() {
	press(CROSS_SCREEN_EDIT);
	press(MIDI);
}
} // namespace
TEST_GROUP(LoopHorizontalSettings){};

// Catches accidental list navigation, wrong physical addressing and eager binding.
TEST(LoopHorizontalSettings, EveryTrackOpensCorrectHorizontalContext) {
	for (SlotId slot = 0; slot < 8; ++slot) {
		Fixture f(true);
		f.sharedShift(f.view, true);
		f.view.padAction(centres[slot][0], centres[slot][1], 100);
		f.sharedShift(loopStationSettingsMenu, false);
		POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
		f.onlyBound(slot);
		CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());
		CHECK_FALSE(f.model->engine().state(slot).capture);
		turn(1);
		for (SlotId other = 0; other < 8; ++other)
			CHECK_EQUAL(int(other == slot ? Source::Right : Source::Left),
			            int(f.model->engine().settings().tracks[other].source));
		press(BACK);
	}
}
TEST(LoopHorizontalSettings, InstrumentButtonsSelectAndSelectRotationEdits) {
	Fixture f(true);
	CHECK_TRUE(loopStationSettingsMenu.openTrack(2));
	press(KIT);
	turn(1);
	CHECK(Monitoring::Off == f.model->engine().settings().tracks[2].monitoring);
	press(MIDI);
	turn(1);
	CHECK_EQUAL(1, f.model->engine().settings().tracks[2].recordDurationBars);
	press(CV);
	turn(1);
	CHECK(PlaybackMode::OneShot == f.model->engine().settings().tracks[2].playback);
	press(CROSS_SCREEN_EDIT);
	turn(1); // shorter page clamps column 4 to Copy
	CHECK(MenuField::CopyToTrack == loopStationSettingsMenu.horizontalMenu_.selectedField());
	press(CV); // empty fourth column must not select or execute
	CHECK(MenuField::CopyToTrack == loopStationSettingsMenu.horizontalMenu_.selectedField());
	press(SYNTH);
	turn(1);
	CHECK_EQUAL(100, f.model->engine().settings().tracks[2].transposeCents);
	press(KIT);
	turn(1);
	CHECK(Colour::Purple == f.model->engine().settings().tracks[2].colour);
	press(CROSS_SCREEN_EDIT);
	turn(1); // wraps to page 1, column 2
	CHECK(Monitoring::On == f.model->engine().settings().tracks[2].monitoring);
	loopStationSettingsMenu.horizontalEncoderAction(1);
	CHECK(Monitoring::On == f.model->engine().settings().tracks[2].monitoring);
}
TEST(LoopHorizontalSettings, ConditionalPageChangesKeepSelectedField) {
	Fixture f(true);
	CHECK_TRUE(loopStationSettingsMenu.openGlobal());
	press(KIT);
	turn(1); // start immediate -> synced, inserting interval
	CHECK(ActionMode::Synced == f.model->engine().settings().startAction.mode);
	turn(1);
	CHECK(ActionMode::Immediate == f.model->engine().settings().startAction.mode);
	press(MIDI);
	turn(1); // stop immediate -> synced
	CHECK(ActionMode::Synced == f.model->engine().settings().stopAction.mode);
	turn(1);
	CHECK(ActionMode::Immediate == f.model->engine().settings().stopAction.mode);
	CHECK(Interval::Sixteenth == f.model->engine().settings().startAction.interval);
	CHECK(Interval::Sixteenth == f.model->engine().settings().stopAction.interval);
}
TEST(LoopHorizontalSettings, SettingsIgnorePadsAndDoNotAlterTransport) {
	Fixture f(true);
	f.recordPhrase();
	CHECK_TRUE(loopStationSettingsMenu.openTrack(0));
	for (int y = 0; y < 8; ++y)
		for (int x = 0; x < 18; ++x) {
			loopStationSettingsMenu.padAction(x, y, 100);
			loopStationSettingsMenu.padAction(x, y, 0);
		}
	turn(1); // pads must not toggle editing or move the selected source
	CHECK(Source::Right == f.model->engine().settings().tracks[0].source);
	CHECK_TRUE(f.model->engine().state(0).hasPhrase);
	CHECK_TRUE(f.model->engine().state(0).playing);
	CHECK_FALSE(f.model->engine().state(0).capture);
	CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());
	CHECK_EQUAL(1, f.host.allocationAttempts);
}
TEST(LoopHorizontalSettings, ShiftPagingNeverEntersSoundEditorChain) {
	Fixture f(true);
	soundEditor = {};
	deluge::gui::menu_item::MenuItem foreign;
	deluge::gui::menu_item::HorizontalMenu other({}, {&foreign});
	probeChain = {&other};
	CHECK_TRUE(loopStationSettingsMenu.openTrack(0));
	f.sharedShift(loopStationSettingsMenu, true);
	press(CROSS_SCREEN_EDIT);
	press(SYNTH);
	turn(1);
	CHECK_EQUAL(1, f.model->engine().settings().tracks[0].transposeCents);
	press(SCALE_MODE);
	press(SYNTH);
	turn(1);
	CHECK(Source::Right == f.model->engine().settings().tracks[0].source);
	CHECK_EQUAL(0, soundEditor.entries);
	CHECK_EQUAL(0, soundEditor.padUpdates);
	CHECK_EQUAL(0, soundEditor.navigationDepth);
	probeChain.clear();
}
TEST(LoopHorizontalSettings, TransposeDetentsAreUnaccelerated) {
	Fixture f(true);
	CHECK_TRUE(loopStationSettingsMenu.openTrack(0));
	press(CROSS_SCREEN_EDIT);
	press(SYNTH);
	for (int i = 0; i < 5; ++i) {
		probeMenuTime += .001;
		turn(1);
	}
	CHECK_EQUAL(500, f.model->engine().settings().tracks[0].transposeCents);
	f.sharedShift(loopStationSettingsMenu, true);
	turn(-3);
	CHECK_EQUAL(497, f.model->engine().settings().tracks[0].transposeCents);
	f.sharedShift(loopStationSettingsMenu, false);
	turn(-2);
	CHECK_EQUAL(297, f.model->engine().settings().tracks[0].transposeCents);
	Buttons::selectPressed = true;
	turn(2);
	Buttons::selectPressed = false;
	CHECK_EQUAL(299, f.model->engine().settings().tracks[0].transposeCents);
	turn(127);
	CHECK_EQUAL(2450, f.model->engine().settings().tracks[0].transposeCents);
	turn(-127);
	CHECK_EQUAL(-2450, f.model->engine().settings().tracks[0].transposeCents);
}
TEST(LoopHorizontalSettings, CopyNeedsTwoDistinctSelectPresses) {
	Fixture f(true);
	CHECK_TRUE(loopStationSettingsMenu.openTrack(0));
	turn(1);
	copyColumn();
	// Destination navigation excludes source and wraps through the other seven tracks.
	for (int i = 0; i < 7; ++i)
		turn(1);
	press(SELECT_ENC);
	for (int i = 0; i < 3; ++i)
		press(SELECT_ENC);
	press(MIDI);
	loopStationSettingsMenu.padAction(5, 6, 100);
	CHECK_FALSE(f.model->clip(1));
	release();
	CHECK_FALSE(f.model->clip(1));
	press(SELECT_ENC);
	CHECK(Source::Right == f.model->engine().settings().tracks[1].source);
	CHECK_TRUE(f.model->clip(1));
	STRCMP_EQUAL("Copied", f.screen.popups.back().c_str());
	const auto count = f.screen.popups.size();
	press(SELECT_ENC);
	CHECK_EQUAL(count, f.screen.popups.size());
}
TEST(LoopHorizontalSettings, BackCancelsCopyWithoutClosingSettings) {
	Fixture f(true);
	CHECK_TRUE(loopStationSettingsMenu.openTrack(0));
	turn(1);
	copyColumn();
	press(SELECT_ENC);
	release();
	press(BACK);
	POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
	CHECK_FALSE(f.model->clip(1));
	press(SELECT_ENC); // review again, not confirm
	CHECK_FALSE(f.model->clip(1));
	release();
	press(BACK);
	press(BACK);
	POINTERS_EQUAL(&f.view, getCurrentUI());
}
TEST(LoopHorizontalSettings, DeferredHeldSelectCannotReviewAndConfirm) {
	Fixture f(true);
	CHECK_TRUE(loopStationSettingsMenu.openTrack(0));
	turn(1);
	copyColumn();
	sdRoutineLock = true;
	CHECK(loopStationSettingsMenu.buttonAction(SELECT_ENC, true, true) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK_FALSE(f.model->clip(1));
	sdRoutineLock = false;
	press(SELECT_ENC);
	press(SELECT_ENC);
	CHECK_FALSE(f.model->clip(1));
	release();
	press(SELECT_ENC);
	CHECK(Source::Right == f.model->engine().settings().tracks[1].source);
}
TEST(LoopHorizontalSettings, MenuExitAndFailedOpenRestoreLedsAndParents) {
	Fixture f(true);
	for (int pass = 0; pass < 3; ++pass) {
		CHECK_TRUE(loopStationSettingsMenu.openTrack(0));
		deluge::hid::display::oled_canvas::Canvas canvas;
		loopStationSettingsMenu.renderOLED(canvas);
		CHECK(indicator_leds::states[IndicatorLED::SYNTH]);
		f.sharedShift(loopStationSettingsMenu, true);
		press(SESSION_VIEW);
		POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
		f.sharedShift(loopStationSettingsMenu, false);
		press(pass == 1 ? SESSION_VIEW : BACK);
		POINTERS_EQUAL(&f.view, getCurrentUI());
		for (auto* item : loopStationSettingsMenu.items_.all())
			POINTERS_EQUAL(nullptr, item->parent);
		CHECK_FALSE(indicator_leds::states[IndicatorLED::SYNTH]);
		CHECK_FALSE(indicator_leds::states[IndicatorLED::KIT]);
		CHECK_FALSE(indicator_leds::states[IndicatorLED::SCALE_MODE]);
		CHECK(indicator_leds::states[IndicatorLED::CROSS_SCREEN_EDIT]); // parent restored after cleanup
	}
	f.host.failAllocation = true;
	CHECK_FALSE(loopStationSettingsMenu.openTrack(7));
	POINTERS_EQUAL(&f.view, getCurrentUI());
	for (auto* item : loopStationSettingsMenu.items_.all())
		POINTERS_EQUAL(nullptr, item->parent);
	CHECK(indicator_leds::states[IndicatorLED::CROSS_SCREEN_EDIT]);
	CHECK_FALSE(loopStationSettingsMenu.openTrack(kInvalidSlot));
	// Returning to a normal editor still uses the default host callbacks.
	deluge::gui::menu_item::MenuItem item;
	deluge::gui::menu_item::HorizontalMenu ordinary({}, {&item});
	soundEditor = {};
	ordinary.beginSession(nullptr);
	ordinary.renderOLED();
	probeMenuTime += 1;
	ordinary.buttonAction(SYNTH, true, false);
	CHECK_EQUAL(1, item.presses);
	CHECK_EQUAL(1, item.permissions);
	ordinary.endSession();
}
TEST(LoopHorizontalSettings, CopyFailureIsSettingsOnlyAndHonest) {
	Fixture f(true);
	f.recordPhrase();
	CHECK_TRUE(loopStationSettingsMenu.openTrack(1));
	turn(1); // source track 2
	copyColumn();
	turn(-1); // destination track 1, with existing audio
	std::array<Frame, 8> silence{}, beforeAudio{}, afterAudio{};
	f.model->adapter().renderDry(0, silence, beforeAudio);
	auto* clip = f.model->clip(0);
	clip->name = "Keep me";
	clip->output = 31;
	clip->fx = 42;
	const auto state = f.model->engine().state(0);
	const auto phase = f.model->adapter().phase(0);
	const bool undo = f.model->canUndo(0), redo = f.model->canRedo(0);
	press(SELECT_ENC);
	release();
	sdRoutineLock = true;
	CHECK(loopStationSettingsMenu.buttonAction(SELECT_ENC, true, true) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK(Source::Left == f.model->engine().settings().tracks[0].source);
	turn(1); // SD-locked rotary input cannot mutate settings or review destination
	sdRoutineLock = false;
	press(SELECT_ENC);
	CHECK(Source::Right == f.model->engine().settings().tracks[0].source);
	STRCMP_EQUAL("Copied", f.screen.popups.back().c_str());
	POINTERS_EQUAL(clip, f.model->clip(0));
	STRCMP_EQUAL("Keep me", clip->name.c_str());
	CHECK_EQUAL(31, clip->output);
	CHECK_EQUAL(42, clip->fx);
	CHECK_EQUAL(state.hasPhrase, f.model->engine().state(0).hasPhrase);
	CHECK_EQUAL(state.playing, f.model->engine().state(0).playing);
	CHECK_EQUAL(phase, f.model->adapter().phase(0));
	CHECK_EQUAL(undo, f.model->canUndo(0));
	CHECK_EQUAL(redo, f.model->canRedo(0));
	f.model->adapter().renderDry(0, silence, afterAudio);
	CHECK(beforeAudio[0].l != 0); // comparison must not be two empty buffers
	for (unsigned i = 0; i < 8; ++i) {
		CHECK_EQUAL(beforeAudio[i].l, afterAudio[i].l);
		CHECK_EQUAL(beforeAudio[i].r, afterAudio[i].r);
	}
	// Busy destination: capture remains active, and no false success.
	release();
	auto settings = f.model->engine().settings();
	settings.tracks[1].source = Source::Stereo;
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK_TRUE(f.model->engine().state(0).capture);
	press(SELECT_ENC);
	release();
	const auto popups = f.screen.popups.size();
	press(SELECT_ENC);
	CHECK_EQUAL(popups + 1, f.screen.popups.size());
	STRCMP_EQUAL("Copy busy", f.screen.popups.back().c_str());
	CHECK_TRUE(f.model->engine().state(0).capture);
	CHECK(Source::Right == f.model->engine().settings().tracks[0].source);
	// Unavailable destination: allocation failure must not change its settings.
	release();
	turn(2); // track 4
	f.host.failAllocation = true;
	press(SELECT_ENC);
	release();
	press(SELECT_ENC);
	CHECK_FALSE(f.model->clip(3));
	CHECK(Source::Left == f.model->engine().settings().tracks[3].source);
	STRCMP_EQUAL("Copy busy", f.screen.popups.back().c_str());
}
TEST(LoopHorizontalSettings, FallbackTransposeRetainsFractionAndFineSteps) {
	Fixture f(false);
	auto settings = f.model->engine().settings();
	settings.tracks[0].transposeCents = 123;
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	CHECK_TRUE(loopStationSettingsMenu.openTrack(0));
	loopStationSettingsMenu.horizontalEncoderAction(4); // fallback navigation alias only
	press(SELECT_ENC);
	release();
	turn(1);
	CHECK_EQUAL(223, f.model->engine().settings().tracks[0].transposeCents);
	f.sharedShift(loopStationSettingsMenu, true);
	turn(-2);
	CHECK_EQUAL(221, f.model->engine().settings().tracks[0].transposeCents);
	f.sharedShift(loopStationSettingsMenu, false);
	Buttons::selectPressed = true;
	turn(-1);
	Buttons::selectPressed = false;
	CHECK_EQUAL(220, f.model->engine().settings().tracks[0].transposeCents);
	turn(-1);
	CHECK_EQUAL(120, f.model->engine().settings().tracks[0].transposeCents);
}
TEST(LoopHorizontalSettings, CopyDestinationIsRenderedInsideSharedSlot) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openTrack(0));
	copyColumn();
	turn(2); // literal destination Track 4
	auto& canvas = deluge::hid::display::OLED::main;
	canvas = {};
	loopStationSettingsMenu.renderOLED(canvas);
	bool found = false;
	for (size_t i = 0; i < canvas.lines.size(); ++i) {
		if (canvas.lines[i].find("Track 4") != std::string::npos) {
			CHECK(canvas.textY[i] < OLED_MAIN_HEIGHT_PIXELS - kTextSizeYUpdated);
			found = true;
		}
	}
	CHECK_TRUE(found);
}
