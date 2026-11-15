#include "deluge/loop_station/gesture_rollback.h"
#include "deluge/loop_station/persistence.h"
#include "fixture.h"
#include "gui/views/affect_entire_led.h"

using namespace loop_station;
using namespace deluge::hid::button;
using deluge::gui::menu_item::loop_station::loopStationSettingsMenu;
// These pre-existing list/7SEG tests explicitly exercise the preference-off fallback.
struct Fixture : loop_ui_test::Fixture {
	Fixture() : loop_ui_test::Fixture(false) {}
};

TEST_GROUP(LoopUi){};

namespace {
// Physical centres, not derived from padAddress (the input mapping under test).
constexpr int32_t centres[8][2] = {{1, 6}, {5, 6}, {9, 6}, {13, 6}, {1, 2}, {5, 2}, {9, 2}, {13, 2}};

void selectClick() {
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
	loopStationSettingsMenu.buttonAction(SELECT_ENC, false, false);
}

void openColour(Fixture& f, SlotId slot) {
	f.sharedShift(f.view, true);
	CHECK(f.view.padAction(centres[slot][0], centres[slot][1], 100) == ActionResult::DEALT_WITH);
	POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
	CHECK_EQUAL(slot, loopStationSettingsMenu.slot_);
	f.sharedShift(loopStationSettingsMenu, false);
	CHECK_TRUE(loopStationSettingsMenu.fields_.contains(MenuField::Colour));
	for (unsigned i = 0; i < loopStationSettingsMenu.fields_.count
	                     && loopStationSettingsMenu.fields_.rows[loopStationSettingsMenu.row_] != MenuField::Colour;
	     ++i)
		loopStationSettingsMenu.selectEncoderAction(1);
	CHECK(MenuField::Colour == loopStationSettingsMenu.fields_.rows[loopStationSettingsMenu.row_]);
	selectClick();
	CHECK_TRUE(loopStationSettingsMenu.editing_);
}

void expectTrackSettings(const TrackSettings& expected, const TrackSettings& actual) {
	CHECK_EQUAL(int(expected.source), int(actual.source));
	CHECK_EQUAL(int(expected.monitoring), int(actual.monitoring));
	CHECK_EQUAL(expected.recordDurationBars, actual.recordDurationBars);
	CHECK_EQUAL(int(expected.playback), int(actual.playback));
	CHECK_EQUAL(int(expected.colour), int(actual.colour));
	CHECK_EQUAL(expected.transposeCents, actual.transposeCents);
}

struct RenderedPads {
	RGB image[kDisplayHeight][kDisplayWidth + kSideBarWidth]{};
	uint8_t mask[kDisplayHeight][kDisplayWidth + kSideBarWidth]{};
	explicit RenderedPads(Fixture& f) { CHECK_TRUE(f.view.renderMainPads(0xff, image, mask, false)); }
};

void expectDelegated(Fixture& f, deluge::hid::Button button, bool on, ActionResult expected) {
	::view.reset();
	::view.result = expected;
	CHECK(f.view.buttonAction(button, on, false) == expected);
	CHECK_EQUAL(1, ::view.calls.size());
	CHECK(button == ::view.calls.front().button);
	CHECK_EQUAL(on, ::view.calls.front().on);
	CHECK_FALSE(::view.calls.front().inCardRoutine);
}
} // namespace

TEST(LoopUi, TrackSettingsPulsesAddressedRingWithoutChangingSelectedTrack) {
	Fixture f;
	f.view.padAction(0, 7, 100);
	CHECK_EQUAL(0, f.model->engine().selected());
	CHECK_TRUE(loopStationSettingsMenu.openTrack(5));
	POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
	probeSampleClock = 0;
	const RenderedPads low(f);
	probeSampleClock = 11025;
	const RenderedPads high(f);
	CHECK(high.image[3][4].r > low.image[3][4].r);      // Track 6 is settings focus.
	CHECK_EQUAL(low.image[7][0].r, high.image[7][0].r); // Engine-selected Track 1 is not.
	CHECK_EQUAL(0, f.model->engine().selected());
	loopStationSettingsMenu.buttonAction(BACK, true, false);
	POINTERS_EQUAL(&f.view, getCurrentUI());
	probeSampleClock = 0;
	const RenderedPads rootLow(f);
	probeSampleClock = 11025;
	const RenderedPads rootHigh(f);
	CHECK(rootHigh.image[7][0].r > rootLow.image[7][0].r);
	CHECK_EQUAL(rootLow.image[3][4].r, rootHigh.image[3][4].r);
}

TEST(LoopUi, SettingsGraphicsTickRequestsOnlyUnderlyingMainPads) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openTrack(5));
	probeRenderTargets.clear();
	probeRenderRows.clear();
	probeOledRequests = 0;
	indicator_leds::states[IndicatorLED::SYNTH] = true;
	for (auto time : {SampleClock{0}, SampleClock{11025}}) {
		probeSampleClock = time;
		getCurrentUI()->graphicsRoutine();
	}
	CHECK_EQUAL(2, probeRenderRows.size());
	for (size_t i = 0; i < probeRenderRows.size(); ++i) {
		POINTERS_EQUAL(&f.view, probeRenderTargets[i]);
		CHECK_EQUAL(0xff, probeRenderRows[i].first);
		CHECK_EQUAL(0, probeRenderRows[i].second);
	}
	CHECK_EQUAL(0, probeOledRequests);
	CHECK_TRUE(indicator_leds::states[IndicatorLED::SYNTH]);
}

TEST(LoopUi, GlobalAndFailedTrackSettingsKeepSelectedFocus) {
	Fixture f;
	f.view.padAction(0, 7, 100);
	CHECK_TRUE(loopStationSettingsMenu.openGlobal());
	CHECK_EQUAL(kInvalidSlot, loopStationSettingsMenu.focusedTrack());
	probeSampleClock = 0;
	const RenderedPads low(f);
	probeSampleClock = 11025;
	const RenderedPads high(f);
	CHECK(high.image[7][0].r > low.image[7][0].r);
	loopStationSettingsMenu.buttonAction(BACK, true, false);
	f.host.failAllocation = true;
	CHECK_FALSE(loopStationSettingsMenu.openTrack(6));
	POINTERS_EQUAL(&f.view, getCurrentUI());
	CHECK_EQUAL(kInvalidSlot, loopStationSettingsMenu.focusedTrack());
	CHECK_EQUAL(0, f.model->engine().selected());
}

TEST(LoopUi, FallbackCopyReviewKeepsAddressedFocusAndPadsOnlyTick) {
	Fixture f;
	f.view.padAction(0, 7, 100);
	CHECK_TRUE(loopStationSettingsMenu.openTrack(5));
	CHECK_EQUAL(5, loopStationSettingsMenu.focusedTrack());
	loopStationSettingsMenu.requestCopyReview();
	probeRenderTargets.clear();
	probeRenderRows.clear();
	probeOledRequests = 0;
	getCurrentUI()->graphicsRoutine();
	CHECK_EQUAL(5, loopStationSettingsMenu.focusedTrack());
	CHECK_EQUAL(1, probeRenderRows.size());
	CHECK_EQUAL(0xff, probeRenderRows.front().first);
	CHECK_EQUAL(0, probeRenderRows.front().second);
	CHECK_EQUAL(0, probeOledRequests);
}

TEST(LoopUi, ForegroundGraphicsDispatchTransmitsAnimatedPadFrames) {
	for (bool horizontal : {true, false}) {
		loop_ui_test::Fixture f(horizontal);
		f.view.padAction(0, 7, 100);
		CHECK_TRUE(loopStationSettingsMenu.openTrack(5));
		if (!horizontal)
			loopStationSettingsMenu.requestCopyReview();
		probeSentPadFrames.clear();
		probeOledRequests = 0;
		indicator_leds::states[IndicatorLED::SYNTH] = true;
		const auto audioTime = f.host.time;
		probeSampleClock = 0;
		probeDispatchGraphicsTick(true);
		CHECK_EQUAL(1, probeSentPadFrames.size());
		probeSampleClock = 11025;
		probeDispatchGraphicsTick(false); // UART backpressure cannot transmit this phase.
		CHECK_EQUAL(1, probeSentPadFrames.size());
		probeDispatchGraphicsTick(true);
		CHECK_EQUAL(2, probeSentPadFrames.size());
		CHECK(probeSentPadFrames[1].focused.r > probeSentPadFrames[0].focused.r);
		CHECK_EQUAL(probeSentPadFrames[0].other.r, probeSentPadFrames[1].other.r);
		CHECK_EQUAL(0, probeOledRequests);
		CHECK_TRUE(indicator_leds::states[IndicatorLED::SYNTH]);
		CHECK_EQUAL(audioTime, f.host.time); // No transport started by display ticks.
	}
}

// Catches wrong-track edits and menu-only invalidation that leaves the root ring stale.
TEST(LoopUi, EveryTrackConfigOffersColourAndChangesOnlyItsRing) {
	constexpr Colour next[] = {Colour::Lime, Colour::Sky,   Colour::Purple, Colour::Magenta,
	                           Colour::Pink, Colour::White, Colour::Yellow, Colour::Turquoise};
	for (SlotId slot = 0; slot < 8; ++slot) {
		Fixture f;
		openColour(f, slot);
		f.onlyBound(slot); // Opening config retains lazy per-track binding and never captures.
		CHECK_FALSE(f.model->engine().state(slot).capture);
		std::array<AudioClip*, 8> clips{};
		for (SlotId other = 0; other < 8; ++other) {
			CHECK(Status::Ok == f.model->ensureBound(other));
			clips[other] = f.model->clip(other);
		}
		auto expected = f.model->engine().settings();
		expected.tracks[slot].colour = next[slot];
		const RenderedPads before(f);
		probeRenderTargets.clear();
		probeOledRequests = 0;
		loopStationSettingsMenu.selectEncoderAction(1);
		expectTrackSettings(expected.tracks[slot], f.model->engine().settings().tracks[slot]);
		const RenderedPads after(f);
		for (int32_t y = 0; y < 8; ++y) {
			for (int32_t x = 0; x < 16; ++x) {
				const int32_t left = centres[slot][0] - 1, bottom = centres[slot][1] - 2;
				const bool tile = x >= left && x < left + 4 && y >= bottom && y < bottom + 4;
				const bool border = tile && (x == left || x == left + 3 || y == bottom || y == bottom + 3);
				CHECK_EQUAL(!border, before.image[y][x] == after.image[y][x]);
			}
		}
		CHECK(std::find(probeRenderTargets.begin(), probeRenderTargets.end(), &f.view) != probeRenderTargets.end());
		CHECK(probeOledRequests > 0);
		selectClick();
		CHECK_FALSE(loopStationSettingsMenu.editing_);
		loopStationSettingsMenu.buttonAction(BACK, true, false);
		POINTERS_EQUAL(&f.view, getCurrentUI());
		for (SlotId other = 0; other < 8; ++other) {
			expectTrackSettings(expected.tracks[other], f.model->engine().settings().tracks[other]);
			POINTERS_EQUAL(clips[other], f.model->clip(other));
		}
		CHECK_EQUAL(8, f.host.allocationAttempts);
	}
}

TEST(LoopUi, ColourPaletteWrapsAndBackKeepsChoice) {
	Fixture f;
	openColour(f, 6); // Gold is the seventh track's default.
	CHECK(Colour::Gold == f.model->engine().settings().tracks[6].colour);
	loopStationSettingsMenu.selectEncoderAction(-1);
	CHECK(Colour::White == f.model->engine().settings().tracks[6].colour);
	loopStationSettingsMenu.selectEncoderAction(1);
	CHECK(Colour::Gold == f.model->engine().settings().tracks[6].colour);
	loopStationSettingsMenu.selectEncoderAction(-1);
	selectClick();
	loopStationSettingsMenu.buttonAction(BACK, true, false);
	POINTERS_EQUAL(&f.view, getCurrentUI());
	CHECK(Colour::White == f.model->engine().settings().tracks[6].colour);
	const RenderedPads pads(f);
	CHECK(pads.image[3][8] == RGB(255, 255, 255).dim(3));
}

TEST(LoopUi, ColourEditingKeepsRecordingAndErrorOverrides) {
	Fixture f;
	f.view.padAction(5, 2, 100);
	CHECK_TRUE(f.model->engine().state(5).capture);
	openColour(f, 5);
	loopStationSettingsMenu.selectEncoderAction(1);
	probeSampleClock = 11025; // Recording pulse and error flash are on.
	const RenderedPads recording(f);
	CHECK(recording.image[3][4] == RGB(255, 0, 0));
	CHECK(recording.image[2][5] == RGB(255, 0, 0));
	Command error{CommandType::CaptureError, 5};
	error.status = Status::SourceUnavailable;
	CHECK(Status::SourceUnavailable == f.model->dispatch(error));
	loopStationSettingsMenu.selectEncoderAction(1);
	CHECK(Status::SourceUnavailable == f.model->engine().state(5).error);
	const RenderedPads failed(f);
	CHECK(failed.image[3][4] == RGB(255, 0, 0).dim(3));
	CHECK(failed.image[2][5] == RGB(255, 0, 0));
}

TEST(LoopUi, ConfiguredTrackColoursRoundTripAfterEditing) {
	Fixture f;
	constexpr Colour chosen[] = {Colour::White, Colour::Gold,  Colour::Turquoise, Colour::Sky,
	                             Colour::Lime,  Colour::White, Colour::Pink,      Colour::Cyan};
	for (SlotId slot = 0; slot < 8; ++slot) {
		openColour(f, slot);
		// Navigate the real picker; assert against literal choices, not the picker result itself.
		const auto current = f.model->engine().settings().tracks[slot].colour;
		const auto from = std::find(kTrackColours.begin(), kTrackColours.end(), current);
		const auto to = std::find(kTrackColours.begin(), kTrackColours.end(), chosen[slot]);
		loopStationSettingsMenu.selectEncoderAction(static_cast<int8_t>(to - from));
		CHECK(chosen[slot] == f.model->engine().settings().tracks[slot].colour);
		selectClick();
		loopStationSettingsMenu.buttonAction(BACK, true, false);
	}
	SettingsDocument document{};
	CHECK(Status::Ok == Persistence::encode(f.model->engine().settings(), Manifest{}, document));
	Settings restored{};
	Manifest manifest{};
	CHECK(Status::Ok == Persistence::decode(document, restored, manifest));
	for (SlotId slot = 0; slot < 8; ++slot) {
		CHECK(chosen[slot] == restored.tracks[slot].colour);
		expectTrackSettings(f.model->engine().settings().tracks[slot], restored.tracks[slot]);
	}
}

// Missing preparation must fail through real SongModel bindings and Engine state,
// not merely through a call count on a stand-in model.
TEST(LoopUi, FreshCentrePressBindsOnlyItsSlotAndStartsCapture) {
	Fixture f;
	CHECK_EQUAL(0, f.host.allocationAttempts);
	CHECK(f.view.padAction(9, 2, 100) == ActionResult::DEALT_WITH);
	f.onlyBound(6);
	CHECK_TRUE(f.model->engine().state(6).capture);
}

TEST(LoopUi, BorderPreparesAndTogglesSelectionWithoutCapture) {
	Fixture f;
	f.view.padAction(12, 3, 100);
	f.onlyBound(7);
	CHECK_EQUAL(7, f.model->engine().selected());
	CHECK_FALSE(f.model->engine().state(7).capture);
	f.view.padAction(12, 3, 0);
	CHECK_EQUAL(7, f.model->engine().selected());
	f.view.padAction(12, 3, 100);
	CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());
	CHECK_EQUAL(1, f.host.allocationAttempts);
}

TEST(LoopUi, EncoderPreparesIntendedSlotWithoutLoopCapture) {
	Fixture f;
	f.view.selectEncoderAction(2);
	f.onlyBound(2);
	CHECK_EQUAL(1, f.host.allocationAttempts);
	CHECK_EQUAL(2, f.model->engine().selected());
	CHECK_FALSE(f.model->engine().state(2).capture);
	f.view.selectEncoderAction(-1);
	CHECK_EQUAL(1, f.model->engine().selected());
	CHECK_TRUE(f.model->clip(1));
	CHECK_EQUAL(2, f.host.allocationAttempts);
	CHECK_FALSE(f.model->engine().state(1).capture);
}

TEST(LoopUi, RecordEdgesReachNativeHandlerWithoutLoopCapture) {
	Fixture f;
	f.view.selectEncoderAction(2);
	f.onlyBound(2);
	CHECK_FALSE(f.model->engine().state(2).capture);
	const auto allocations = f.host.allocationAttempts;
	expectDelegated(f, RECORD, true, ActionResult::NOT_DEALT_WITH);
	expectDelegated(f, RECORD, false, ActionResult::NOT_DEALT_WITH);
	CHECK_EQUAL(2, f.model->engine().selected());
	CHECK_FALSE(f.model->engine().state(2).capture);
	CHECK_EQUAL(allocations, f.host.allocationAttempts);
}

TEST(LoopUi, RecordEdgesDoNotBindOrCaptureEngineSelectedUnboundSlot) {
	Fixture f;
	// Engine selection may exist independently of a firmware clip binding.
	CHECK(Status::Ok == f.model->engine().dispatch({CommandType::Select, 3}, f.host.time, f.model->clock()));
	expectDelegated(f, RECORD, true, ActionResult::NOT_DEALT_WITH);
	expectDelegated(f, RECORD, false, ActionResult::NOT_DEALT_WITH);
	CHECK_EQUAL(3, f.model->engine().selected());
	CHECK_FALSE(f.model->clip(3));
	CHECK_FALSE(f.model->engine().state(3).capture);
	CHECK_EQUAL(0, f.host.allocationAttempts);
}

TEST(LoopUi, RepeatedCentreUseKeepsClipIdentity) {
	Fixture f;
	f.view.padAction(1, 6, 100);
	f.onlyBound(0);
	auto* clip = f.model->clip(0);
	f.view.padAction(1, 6, 0);
	f.view.padAction(1, 6, 100);
	POINTERS_EQUAL(clip, f.model->clip(0));
	CHECK_EQUAL(1, f.host.allocationAttempts);
}

TEST(LoopUi, UnboundReleaseAndGroupControlsAllocateNothing) {
	Fixture f;
	f.view.padAction(5, 6, 0);
	f.view.padAction(4, 7, 0);
	f.view.padAction(16, 7, 100);
	f.view.padAction(17, 7, 100);
	f.view.buttonAction(PLAY, true, false);
	f.view.buttonAction(RECORD, false, false);
	CHECK_EQUAL(0, f.host.allocationAttempts);
	CHECK_EQUAL(0, f.host.published.size());
}

TEST(LoopUi, PreparationFailureShowsPopupAndCentreRetryCaptures) {
	Fixture f;
	f.host.failAllocation = true;
	f.view.padAction(5, 2, 100);
	CHECK_EQUAL(1, f.host.allocationAttempts);
	CHECK_FALSE(f.model->clip(5));
	CHECK_FALSE(f.model->engine().state(5).capture);
	CHECK_EQUAL(1, f.screen.popups.size());
	STRCMP_EQUAL("Track Unavailable", f.screen.popups.back().c_str());
	f.host.failAllocation = false;
	f.view.padAction(5, 2, 100);
	f.onlyBound(5);
	CHECK_TRUE(f.model->engine().state(5).capture);
}

TEST(LoopUi, FailedSelectionDoesNotChangeSelectionAndRetries) {
	Fixture f;
	f.host.failAllocation = true;
	f.view.padAction(4, 7, 100);
	CHECK_EQUAL(1, f.screen.popups.size());
	CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());
	f.host.failAllocation = false;
	f.view.padAction(4, 7, 100);
	f.onlyBound(1);
	CHECK_EQUAL(1, f.model->engine().selected());
}

TEST(LoopUi, LockedCentreDefersAllocationThenRetryCaptures) {
	Fixture f;
	sdRoutineLock = true;
	CHECK(f.view.padAction(1, 6, 100) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK_EQUAL(0, f.host.allocationAttempts);
	sdRoutineLock = false;
	f.view.padAction(1, 6, 100);
	f.onlyBound(0);
	CHECK_TRUE(f.model->engine().state(0).capture);
}

TEST(LoopUi, LockedBorderDefersSelectionThenRetryPrepares) {
	Fixture f;
	sdRoutineLock = true;
	CHECK(f.view.padAction(8, 3, 100) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK_EQUAL(0, f.host.allocationAttempts);
	CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());
	sdRoutineLock = false;
	f.view.padAction(8, 3, 100);
	f.onlyBound(6);
	CHECK_EQUAL(6, f.model->engine().selected());
}

TEST(LoopUi, LockedShiftCentreDefersMenuAndAllocationThenRetryOpensTrack) {
	Fixture f;
	Buttons::shiftPressed = true;
	sdRoutineLock = true;
	CHECK(f.view.padAction(13, 6, 100) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK_EQUAL(0, f.host.allocationAttempts);
	POINTERS_EQUAL(&f.view, getCurrentUI());
	sdRoutineLock = false;
	f.view.padAction(13, 6, 100);
	f.onlyBound(3);
	POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
	CHECK_EQUAL(3, loopStationSettingsMenu.slot_);
	CHECK_FALSE(f.model->engine().state(3).capture);
}

TEST(LoopUi, LockedUnboundReleaseDoesNotAllocateOrDefer) {
	Fixture f;
	sdRoutineLock = true;
	CHECK(f.view.padAction(1, 6, 0) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(0, f.host.allocationAttempts);
}

TEST(LoopUi, PerformanceShiftEdgesReachSharedProcessingAndOpenTrackSettings) {
	Fixture f;
	CHECK(f.sharedShift(f.view, true) == ActionResult::NOT_DEALT_WITH);
	CHECK_TRUE(Buttons::shiftPressed);
	f.view.padAction(5, 6, 100);
	POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
	f.onlyBound(1);
	CHECK_FALSE(f.model->engine().state(1).capture);
	CHECK(f.sharedShift(f.view, false) == ActionResult::NOT_DEALT_WITH);
	CHECK_FALSE(Buttons::shiftPressed);
}

TEST(LoopUi, SettingsShiftEdgesReachSharedProcessing) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openGlobal());
	CHECK(f.sharedShift(loopStationSettingsMenu, true) == ActionResult::NOT_DEALT_WITH);
	CHECK_TRUE(Buttons::shiftPressed);
	CHECK(f.sharedShift(loopStationSettingsMenu, false) == ActionResult::NOT_DEALT_WITH);
	CHECK_FALSE(Buttons::shiftPressed);
	POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
	CHECK_EQUAL(0, f.host.allocationAttempts);
}

TEST(LoopUi, UnhandledRootButtonsDelegateAndCardRoutineDefers) {
	Fixture f;
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.view.buttonAction(SYNTH, true, false) == ActionResult::NOT_DEALT_WITH);
	CHECK_EQUAL(1, ::view.calls.size());
	CHECK(SYNTH == ::view.calls.front().button);
	CHECK_TRUE(::view.calls.front().on);
	CHECK_FALSE(::view.calls.front().inCardRoutine);
	::view.reset();
	CHECK(f.view.buttonAction(RECORD, true, true) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK_EQUAL(0, ::view.calls.size());
	CHECK_EQUAL(0, f.host.allocationAttempts);
}

TEST(LoopUi, SettingsMenuCardRoutineDeferralIsPreserved) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openGlobal());
	CHECK(loopStationSettingsMenu.buttonAction(SELECT_ENC, true, true) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK_EQUAL(0, f.host.allocationAttempts);
}

TEST(LoopUi, SharedButtonsDelegateExactEdgesAndResults) {
	Fixture f;
	expectDelegated(f, TAP_TEMPO, true, ActionResult::DEALT_WITH);
	expectDelegated(f, TAP_TEMPO, false, ActionResult::NOT_DEALT_WITH);
	expectDelegated(f, SAVE, true, ActionResult::DEALT_WITH);
	expectDelegated(f, SAVE, false, ActionResult::DEALT_WITH);
	expectDelegated(f, LOAD, true, ActionResult::DEALT_WITH);
	expectDelegated(f, LOAD, false, ActionResult::NOT_DEALT_WITH);
	expectDelegated(f, TEMPO_ENC, true, ActionResult::DEALT_WITH);
	expectDelegated(f, TEMPO_ENC, false, ActionResult::DEALT_WITH);

	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.sharedShift(f.view, true) == ActionResult::NOT_DEALT_WITH);
	CHECK_TRUE(Buttons::shiftPressed);
	expectDelegated(f, SELECT_ENC, true, ActionResult::DEALT_WITH);
	expectDelegated(f, SELECT_ENC, false, ActionResult::NOT_DEALT_WITH);
	CHECK(f.sharedShift(f.view, false) == ActionResult::NOT_DEALT_WITH);
	CHECK_FALSE(Buttons::shiftPressed);
}

TEST(LoopUi, TransportEdgesReachNativeHandlerWithoutLoopActions) {
	Fixture f;
	f.recordPhrase();
	f.view.padAction(17, 7, 100);
	CHECK_FALSE(f.model->engine().state(0).playing);
	auto allocations = f.host.allocationAttempts;

	expectDelegated(f, PLAY, true, ActionResult::NOT_DEALT_WITH);
	expectDelegated(f, PLAY, false, ActionResult::NOT_DEALT_WITH);
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK_EQUAL(allocations, f.host.allocationAttempts);

	// Engine selection may exist independently of a firmware clip binding.
	CHECK(Status::Ok == f.model->engine().dispatch({CommandType::Select, 3}, f.host.time, f.model->clock()));
	expectDelegated(f, RECORD, true, ActionResult::NOT_DEALT_WITH);
	expectDelegated(f, RECORD, false, ActionResult::NOT_DEALT_WITH);
	CHECK_EQUAL(3, f.model->engine().selected());
	CHECK_FALSE(f.model->clip(3));
	CHECK_FALSE(f.model->engine().state(3).capture);
	CHECK_EQUAL(allocations, f.host.allocationAttempts);

	expectDelegated(f, SHIFT, true, ActionResult::NOT_DEALT_WITH);
	expectDelegated(f, SHIFT, false, ActionResult::NOT_DEALT_WITH);

	// Native SHIFT + RECORD combinations must survive intact.
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.sharedShift(f.view, true) == ActionResult::NOT_DEALT_WITH);
	expectDelegated(f, RECORD, true, ActionResult::NOT_DEALT_WITH);
	expectDelegated(f, RECORD, false, ActionResult::NOT_DEALT_WITH);
	CHECK_FALSE(f.model->engine().state(3).capture);
	CHECK_FALSE(f.model->clip(3));
	CHECK_EQUAL(allocations, f.host.allocationAttempts);
	CHECK(f.sharedShift(f.view, false) == ActionResult::NOT_DEALT_WITH);
}

TEST(LoopUi, StandardFallbackWithoutSongOrLoopModel) {
	Fixture f;
	::view.result = ActionResult::DEALT_WITH;
	currentSong = nullptr;
	CHECK(f.view.buttonAction(TAP_TEMPO, true, false) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(1, ::view.calls.size());
	CHECK(f.view.padAction(1, 6, 100) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(0, f.host.allocationAttempts);

	currentSong = &f.song;
	f.song.model = nullptr;
	::view.reset();
	::view.result = ActionResult::DEALT_WITH;
	CHECK(f.view.buttonAction(SAVE, true, false) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(1, ::view.calls.size());
	CHECK(f.view.padAction(1, 6, 100) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(0, f.host.allocationAttempts);
}

TEST(LoopUi, SharedModesDoNotTriggerLoopActions) {
	constexpr uint32_t modes[] = {UI_MODE_HOLDING_SAVE_BUTTON, UI_MODE_HOLDING_LOAD_BUTTON, UI_MODE_MIDI_LEARN,
	                              UI_MODE_RECORD_COUNT_IN};
	for (uint32_t mode : modes) {
		Fixture f;
		f.recordPhrase();
		f.view.padAction(17, 7, 100);
		CHECK_FALSE(f.model->engine().state(0).playing);
		const auto allocations = f.host.allocationAttempts;
		const auto selected = f.model->engine().selected();
		currentUIMode = mode;

		// New tile and sidebar presses are ignored without disturbing accepted state.
		CHECK(f.view.padAction(1, 6, 100) == ActionResult::DEALT_WITH);
		CHECK_FALSE(f.model->engine().state(0).capture);
		CHECK_FALSE(f.model->engine().state(0).playing);
		CHECK_EQUAL(allocations, f.host.allocationAttempts);
		f.view.padAction(16, 7, 100);
		CHECK_FALSE(f.model->engine().state(0).playing);

		f.view.selectEncoderAction(1);
		CHECK_EQUAL(selected, f.model->engine().selected());

		::view.reset();
		::view.result = ActionResult::NOT_DEALT_WITH;
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::NOT_DEALT_WITH);
		CHECK_EQUAL(1, ::view.calls.size());
		CHECK(BACK == ::view.calls.front().button);
		CHECK_TRUE(f.view.historyPromptState_ == LoopStationView::HistoryPromptState::None);

		POINTERS_EQUAL(&f.view, getCurrentUI());
		::view.reset();
		::view.result = ActionResult::NOT_DEALT_WITH;
		CHECK(f.view.buttonAction(CROSS_SCREEN_EDIT, true, false) == ActionResult::NOT_DEALT_WITH);
		POINTERS_EQUAL(&f.view, getCurrentUI());
		CHECK_EQUAL(1, ::view.calls.size());

		expectDelegated(f, TAP_TEMPO, true, ActionResult::DEALT_WITH);
		if (mode == UI_MODE_MIDI_LEARN) {
			expectDelegated(f, PLAY, true, ActionResult::DEALT_WITH);
			expectDelegated(f, RECORD, true, ActionResult::DEALT_WITH);
		}
		currentUIMode = UI_MODE_NONE;
	}
}

TEST(LoopUi, StutterAlonePreservesLoopPerformanceAndNavigation) {
	Fixture f;
	f.recordPhrase();
	CHECK_TRUE(f.model->engine().state(0).playing);
	currentUIMode = UI_MODE_STUTTERING;

	// Sidebar Stop All / Start All remain loop controls under a non-exclusive flag.
	f.view.padAction(17, 7, 100);
	CHECK_FALSE(f.model->engine().state(0).playing);
	f.view.padAction(16, 7, 100);
	CHECK_TRUE(f.model->engine().state(0).playing);
	f.view.padAction(17, 7, 100);
	CHECK_FALSE(f.model->engine().state(0).playing);

	// Border preparation and selection are still accepted.
	const auto allocations = f.host.allocationAttempts;
	f.view.padAction(4, 7, 100);
	CHECK_EQUAL(1, f.model->engine().selected());
	CHECK_EQUAL(allocations + 1, f.host.allocationAttempts);
	f.view.selectEncoderAction(-1);
	CHECK_EQUAL(0, f.model->engine().selected());

	// BACK remains a loop shortcut rather than falling into shared handling.
	::view.reset();
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(0, ::view.calls.size());
	// The recovered base recording now offers Undo, so BACK arms the two-press
	// confirmation; cancel it so the rest of this stutter test keeps its intent.
	CHECK(f.view.historyPromptState_ == LoopStationView::HistoryPromptState::Confirm);
	f.view.cancelHistoryConfirmation();

	// A new centre press is accepted and its release starts the stopped phrase.
	f.view.padAction(1, 6, 100);
	CHECK_FALSE(f.model->engine().state(0).playing);
	f.view.padAction(1, 6, 0);
	CHECK_TRUE(f.model->engine().state(0).playing);

	// Clip View and exit retain their loop handlers.
	CHECK(f.view.buttonAction(CLIP_VIEW, true, false) == ActionResult::DEALT_WITH);
	POINTERS_EQUAL(f.model->clip(0), sessionView.viewedClip);
	CHECK(f.view.buttonAction(CROSS_SCREEN_EDIT, true, false) == ActionResult::DEALT_WITH);
	POINTERS_EQUAL(&sessionView, getCurrentUI());
	CHECK_EQUAL(0, ::view.calls.size());
	currentUIMode = UI_MODE_NONE;
}

TEST(LoopUi, StutterWithSharedHoldStillBlocksLoopActions) {
	constexpr uint32_t holds[] = {UI_MODE_HOLDING_SAVE_BUTTON, UI_MODE_MIDI_LEARN};
	for (uint32_t hold : holds) {
		Fixture f;
		f.recordPhrase();
		f.view.padAction(17, 7, 100);
		CHECK_FALSE(f.model->engine().state(0).playing);
		const auto allocations = f.host.allocationAttempts;
		const auto selected = f.model->engine().selected();
		currentUIMode = UI_MODE_STUTTERING | hold;

		CHECK(f.view.padAction(1, 6, 100) == ActionResult::DEALT_WITH);
		CHECK_FALSE(f.model->engine().state(0).capture);
		CHECK_FALSE(f.model->engine().state(0).playing);
		CHECK_EQUAL(allocations, f.host.allocationAttempts);
		f.view.padAction(16, 7, 100);
		CHECK_FALSE(f.model->engine().state(0).playing);
		f.view.selectEncoderAction(1);
		CHECK_EQUAL(selected, f.model->engine().selected());

		::view.reset();
		::view.result = ActionResult::NOT_DEALT_WITH;
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::NOT_DEALT_WITH);
		CHECK_EQUAL(1, ::view.calls.size());
		CHECK(BACK == ::view.calls.front().button);
		CHECK_TRUE(f.view.historyPromptState_ == LoopStationView::HistoryPromptState::None);

		POINTERS_EQUAL(&f.view, getCurrentUI());
		::view.reset();
		::view.result = ActionResult::NOT_DEALT_WITH;
		CHECK(f.view.buttonAction(CROSS_SCREEN_EDIT, true, false) == ActionResult::NOT_DEALT_WITH);
		POINTERS_EQUAL(&f.view, getCurrentUI());
		CHECK_EQUAL(1, ::view.calls.size());
		currentUIMode = UI_MODE_NONE;
	}
}

TEST(LoopUi, AcceptedCentreReleaseCompletesDuringSharedHold) {
	Fixture f;
	f.recordPhrase();
	f.view.padAction(17, 7, 100);
	CHECK_FALSE(f.model->engine().state(0).playing);
	// This press is accepted while idle and latches a held gesture.
	CHECK(f.view.padAction(1, 6, 100) == ActionResult::DEALT_WITH);
	CHECK_FALSE(f.model->engine().state(0).playing);
	currentUIMode = UI_MODE_HOLDING_SAVE_BUTTON;
	f.view.padAction(1, 6, 0);
	CHECK_TRUE(f.model->engine().state(0).playing);
	currentUIMode = UI_MODE_NONE;
}

TEST(LoopUi, SharedModalTakeoverCancelsStoppedCentreHoldWithoutReplay) {
	Fixture f;
	f.recordPhrase();
	f.view.padAction(5, 6, 100);
	f.renderAudio();
	f.view.padAction(5, 6, 0);
	f.view.padAction(5, 6, 100);
	CHECK_TRUE(f.model->engine().state(1).hasPhrase);
	CHECK(Status::Ok == f.model->dispatch({CommandType::StopAll}));
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK_FALSE(f.model->engine().state(1).playing);
	f.view.padAction(1, 6, 100); // Accept a stopped-pad hold.
	f.view.padAction(5, 6, 100); // Simultaneous accepted hold on another pad.
	CHECK(f.model->engine().heldDurationSamples(0, f.host.time + 100) > 0);
	CHECK(f.model->engine().heldDurationSamples(1, f.host.time + 100) > 0);

	CHECK_TRUE(loopStationSettingsMenu.openGlobal()); // Modal now owns physical pad edges.
	CHECK(f.view.buttonAction(LOAD, false, false) == ActionResult::NOT_DEALT_WITH);
	CHECK_EQUAL(0, f.model->engine().heldDurationSamples(0, f.host.time + 2 * loop_station::kSampleRate));
	CHECK_EQUAL(0, f.model->engine().heldDurationSamples(1, f.host.time + 2 * loop_station::kSampleRate));
	// Even a delayed release routed back to the old view cannot start/clear it.
	CHECK(Status::Ok == f.model->dispatch({CommandType::Release, 0}));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Release, 1}));
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK_FALSE(f.model->engine().state(1).playing);
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->engine().state(1).hasPhrase);
	loopStationSettingsMenu.close();
}

TEST(LoopUi, SharedModalTakeoverCancelsLongStoppedCentreHoldWithoutClear) {
	Fixture f;
	f.recordPhrase();
	CHECK(Status::Ok == f.model->dispatch({CommandType::StopAll}));
	CHECK_FALSE(f.model->engine().state(0).playing);
	f.view.padAction(1, 6, 100);                      // Accept a stopped-pad hold.
	f.host.time += 2 * loop_station::kSampleRate + 1; // A release now would clear the phrase.
	CHECK(f.model->engine().heldDurationSamples(0, f.host.time) >= 2 * loop_station::kSampleRate);

	CHECK_TRUE(loopStationSettingsMenu.openGlobal()); // Modal now owns physical pad edges.
	CHECK(f.view.buttonAction(LOAD, false, false) == ActionResult::NOT_DEALT_WITH);
	CHECK_EQUAL(0, f.model->engine().heldDurationSamples(0, f.host.time));
	// The long hold's release would have cleared the phrase; after takeover it must not.
	CHECK(Status::Ok == f.model->dispatch({CommandType::Release, 0}));
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK(f.model->engine().state(0).hasPhrase);
	loopStationSettingsMenu.close();
}

TEST(LoopUi, UnshiftedSelectOpensGlobalSettingsAndShiftedSelectDelegates) {
	{
		Fixture f;
		CHECK(f.view.buttonAction(SELECT_ENC, true, false) == ActionResult::DEALT_WITH);
		POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
		CHECK_EQUAL(kInvalidSlot, loopStationSettingsMenu.focusedTrack());
		CHECK(f.view.buttonAction(SELECT_ENC, false, false) == ActionResult::DEALT_WITH);
		CHECK_EQUAL(0, ::view.calls.size());
	}
	{
		Fixture f;
		::view.result = ActionResult::NOT_DEALT_WITH;
		CHECK(f.sharedShift(f.view, true) == ActionResult::NOT_DEALT_WITH);
		CHECK_TRUE(Buttons::shiftPressed);
		expectDelegated(f, SELECT_ENC, true, ActionResult::DEALT_WITH);
		POINTERS_EQUAL(&f.view, getCurrentUI());
		::view.result = ActionResult::NOT_DEALT_WITH;
		CHECK(f.sharedShift(f.view, false) == ActionResult::NOT_DEALT_WITH);
		CHECK_FALSE(Buttons::shiftPressed);
	}
}

TEST(LoopUi, ShiftCentreOpensTrackSettingsWithoutCapture) {
	Fixture f;
	CHECK(f.sharedShift(f.view, true) == ActionResult::NOT_DEALT_WITH);
	CHECK(f.view.padAction(13, 6, 100) == ActionResult::DEALT_WITH);
	POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
	CHECK_EQUAL(3, loopStationSettingsMenu.slot_);
	CHECK_FALSE(f.model->engine().state(3).capture);
	f.onlyBound(3);
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.sharedShift(f.view, false) == ActionResult::NOT_DEALT_WITH);
}

TEST(LoopUi, SidebarStartAllAndStopAllRemainLoopControls) {
	Fixture f;
	f.recordPhrase();
	f.view.padAction(17, 7, 100);
	CHECK_FALSE(f.model->engine().state(0).playing);
	f.view.padAction(16, 7, 100);
	CHECK_TRUE(f.model->engine().state(0).playing);
}

TEST(LoopUi, ClipViewAndExitRemainIdleLoopNavigation) {
	Fixture f;
	CHECK(Status::Ok == f.model->ensureBound(0));
	f.view.padAction(0, 7, 100);
	CHECK_EQUAL(0, f.model->engine().selected());
	ParamManagerForTimeline sourceParams;
	constexpr int32_t rateParam = deluge::modulation::params::UNPATCHED_STUTTER_RATE;
	constexpr int32_t sourceRate = 20 << 25;
	sourceParams.params.params[rateParam].value = sourceRate;
	ProbeAudioModControllable source;
	view.activeModControllableModelStack.modControllable = &source;
	view.activeModControllableModelStack.paramManager = &sourceParams;
	CHECK(Error::NONE
	      == stutterer.beginStutter(&source, &sourceParams, StutterConfig{.quantized = true}, 13, 0x40000000));
	currentUIMode = UI_MODE_STUTTERING;
	CHECK(f.view.buttonAction(CLIP_VIEW, true, false) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(sourceRate, sourceParams.params.getValue(rateParam));
	CHECK_FALSE(currentUIMode & UI_MODE_STUTTERING);
	CHECK_TRUE(targetChangeView.pendingStutterEncoderRelease_);
	POINTERS_EQUAL(f.model->clip(0), sessionView.viewedClip);
	CHECK(f.view.buttonAction(CROSS_SCREEN_EDIT, true, false) == ActionResult::DEALT_WITH);
	POINTERS_EQUAL(&sessionView, getCurrentUI());
	CHECK_EQUAL(0, ::view.calls.size());
}

TEST(LoopUi, StutterIsRestoredBeforeSuccessfulSessionExit) {
	Fixture f;
	CHECK(Status::Ok == f.model->ensureBound(0));
	f.view.padAction(0, 7, 100);
	ParamManagerForTimeline sourceParams;
	constexpr int32_t rateParam = deluge::modulation::params::UNPATCHED_STUTTER_RATE;
	constexpr int32_t sourceRate = -(18 << 25);
	sourceParams.params.params[rateParam].value = sourceRate;
	ProbeAudioModControllable source;
	view.activeModControllableModelStack.modControllable = &source;
	view.activeModControllableModelStack.paramManager = &sourceParams;
	CHECK(Error::NONE
	      == stutterer.beginStutter(&source, &sourceParams, StutterConfig{.quantized = true}, 13, 0x40000000));
	currentUIMode = UI_MODE_STUTTERING;
	CHECK(f.view.buttonAction(CROSS_SCREEN_EDIT, true, false) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(sourceRate, sourceParams.params.getValue(rateParam));
	CHECK_FALSE(currentUIMode & UI_MODE_STUTTERING);
	CHECK_TRUE(targetChangeView.pendingStutterEncoderRelease_);
	POINTERS_EQUAL(&sessionView, getCurrentUI());
}

TEST(LoopUi, RepeatedCardRoutineRetriesDoNotDuplicateActions) {
	Fixture f;
	sdRoutineLock = true;
	for (int i = 0; i < 3; ++i)
		CHECK(f.view.padAction(1, 6, 100) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK_EQUAL(0, f.host.allocationAttempts);
	CHECK_FALSE(f.model->engine().state(0).capture);
	sdRoutineLock = false;
	f.view.padAction(1, 6, 100);
	f.onlyBound(0);
	CHECK_TRUE(f.model->engine().state(0).capture);
	CHECK_EQUAL(1, f.host.allocationAttempts);
	CHECK_EQUAL(1, f.host.published.size());
}

// Each regression pairs the real handler's state transition with its OLED request;
// pad-only invalidation must not satisfy the display contract.
TEST(LoopUi, SelectTurnMovesSettingsRowAndInvalidatesOled) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openGlobal());
	probeOledRequests = 0;
	loopStationSettingsMenu.selectEncoderAction(1);
	CHECK_EQUAL(1, loopStationSettingsMenu.row_);
	CHECK_FALSE(loopStationSettingsMenu.editing_);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, SelectClickEntersEditingAndInvalidatesOled) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openGlobal());
	probeOledRequests = 0;
	selectClick();
	CHECK_TRUE(loopStationSettingsMenu.editing_);
	CHECK_EQUAL(0, loopStationSettingsMenu.row_);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, SelectClickLeavesEditingAndInvalidatesOled) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openGlobal());
	selectClick();
	probeOledRequests = 0;
	selectClick();
	CHECK_FALSE(loopStationSettingsMenu.editing_);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, EditingSelectTurnChangesValueAndInvalidatesOled) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openTrack(2));
	CHECK(Source::Left == f.model->engine().settings().tracks[2].source);
	selectClick();
	probeOledRequests = 0;
	loopStationSettingsMenu.selectEncoderAction(1);
	CHECK(Source::Right == f.model->engine().settings().tracks[2].source);
	CHECK_TRUE(loopStationSettingsMenu.editing_);
	CHECK_EQUAL(0, loopStationSettingsMenu.row_);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, CopyTargetEntryInvalidatesOled) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openTrack(2));
	loopStationSettingsMenu.selectEncoderAction(6);
	probeOledRequests = 0;
	selectClick();
	CHECK_TRUE(loopStationSettingsMenu.choosingCopyTarget_);
	CHECK_EQUAL(3, loopStationSettingsMenu.copyTarget_);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, CopyTargetMovementInvalidatesOled) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openTrack(2));
	loopStationSettingsMenu.selectEncoderAction(6);
	selectClick();
	probeOledRequests = 0;
	loopStationSettingsMenu.selectEncoderAction(1);
	CHECK_TRUE(loopStationSettingsMenu.choosingCopyTarget_);
	CHECK_EQUAL(4, loopStationSettingsMenu.copyTarget_);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, CopyConfirmationCopiesSettingsAndInvalidatesOled) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openTrack(2));
	selectClick();
	loopStationSettingsMenu.selectEncoderAction(1);
	selectClick();
	loopStationSettingsMenu.selectEncoderAction(6);
	selectClick(); // destination chooser in fallback
	selectClick(); // review the chosen destination
	probeOledRequests = 0;
	selectClick(); // distinct confirming press
	CHECK_FALSE(loopStationSettingsMenu.choosingCopyTarget_);
	CHECK(Source::Right == f.model->engine().settings().tracks[3].source);
	CHECK_TRUE(f.model->clip(3));
	STRCMP_EQUAL("Copied", f.screen.popups.back().c_str());
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, SevenSegmentNavigationAndEditingRefreshTextAndPopup) {
	Fixture f;
	f.screen.oled = false;
	CHECK_TRUE(loopStationSettingsMenu.openGlobal());
	STRCMP_EQUAL("Loop Global", f.screen.text.c_str());
	STRCMP_EQUAL("PLAY DUB", f.screen.popups.back().c_str());
	f.screen.text.clear();
	const auto before = f.screen.popups.size();
	loopStationSettingsMenu.selectEncoderAction(1);
	CHECK_EQUAL(1, loopStationSettingsMenu.row_);
	STRCMP_EQUAL("Loop Global", f.screen.text.c_str());
	STRCMP_EQUAL("IMM", f.screen.popups.back().c_str());
	selectClick();
	CHECK_TRUE(loopStationSettingsMenu.editing_);
	CHECK_EQUAL(before + 2, f.screen.popups.size());
	loopStationSettingsMenu.selectEncoderAction(1);
	CHECK(ActionMode::Synced == f.model->engine().settings().startAction.mode);
	STRCMP_EQUAL("SYNC", f.screen.popups.back().c_str());
	CHECK_EQUAL(0, probeOledRequests);
}

TEST(LoopUi, SevenSegmentCopyTargetRefreshesAndConfirmationRestoresTrackText) {
	Fixture f;
	f.screen.oled = false;
	CHECK_TRUE(loopStationSettingsMenu.openTrack(2));
	STRCMP_EQUAL("Loop Track", f.screen.text.c_str());
	STRCMP_EQUAL("SRC 1", f.screen.popups.back().c_str());
	loopStationSettingsMenu.selectEncoderAction(6);
	selectClick();
	CHECK_TRUE(loopStationSettingsMenu.choosingCopyTarget_);
	STRCMP_EQUAL("TO TR 4", f.screen.popups.back().c_str());
	loopStationSettingsMenu.selectEncoderAction(1);
	CHECK_EQUAL(4, loopStationSettingsMenu.copyTarget_);
	STRCMP_EQUAL("TO TR 5", f.screen.popups.back().c_str());
	selectClick(); // review before a separate confirming press
	STRCMP_EQUAL("COPY TR 5?", f.screen.popups.back().c_str());
	f.screen.text.clear();
	const auto before = f.screen.popups.size();
	selectClick();
	CHECK_FALSE(loopStationSettingsMenu.choosingCopyTarget_);
	CHECK_TRUE(f.model->clip(4));
	STRCMP_EQUAL("TO TR 5", f.screen.popups[before].c_str());
	STRCMP_EQUAL("Copied", f.screen.popups.back().c_str());
	STRCMP_EQUAL("Loop Track", f.screen.text.c_str());
	CHECK_EQUAL(0, probeOledRequests);
}

TEST(LoopUi, PerformanceCentreCaptureInvalidatesOled) {
	Fixture f;
	probeOledRequests = 0;
	f.view.padAction(1, 6, 100);
	CHECK_TRUE(f.model->engine().state(0).capture);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, PerformanceBorderSelectionInvalidatesOled) {
	Fixture f;
	probeOledRequests = 0;
	f.view.padAction(4, 7, 100);
	CHECK_EQUAL(1, f.model->engine().selected());
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, PerformanceEncoderSelectionInvalidatesOled) {
	Fixture f;
	probeOledRequests = 0;
	f.view.selectEncoderAction(2);
	CHECK_EQUAL(2, f.model->engine().selected());
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, PerformanceRecordDelegationDoesNotStartLoopCapture) {
	Fixture f;
	f.view.selectEncoderAction(2);
	probeOledRequests = 0;
	expectDelegated(f, RECORD, true, ActionResult::NOT_DEALT_WITH);
	CHECK_FALSE(f.model->engine().state(2).capture);
	CHECK_EQUAL(0, probeOledRequests);
}

TEST(LoopUi, PerformanceGroupStopInvalidatesOled) {
	Fixture f;
	f.recordPhrase();
	probeOledRequests = 0;
	f.view.padAction(17, 7, 100);
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, PerformanceGroupStartInvalidatesOled) {
	Fixture f;
	f.recordPhrase();
	f.view.padAction(17, 7, 100);
	probeOledRequests = 0;
	f.view.padAction(16, 7, 100);
	CHECK_TRUE(f.model->engine().state(0).playing);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, PerformancePlayDelegationDoesNotLaunchGroup) {
	Fixture f;
	f.recordPhrase();
	f.view.padAction(17, 7, 100);
	probeOledRequests = 0;
	expectDelegated(f, PLAY, true, ActionResult::NOT_DEALT_WITH);
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK_EQUAL(0, probeOledRequests);
}

TEST(LoopUi, OnlyPhysicalTopSidebarControlsDispatch) {
	Fixture f;
	// Use real commands for setup so a mirrored centre cannot mask sidebar failures.
	CHECK(Status::Ok == f.model->ensureBound(0));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.renderAudio();
	CHECK(Status::Ok == f.model->dispatch({CommandType::Release, 0}));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK_TRUE(f.model->engine().state(0).playing);
	for (int32_t y = 0; y < 7; ++y) {
		f.view.padAction(16, y, 100);
		f.view.padAction(17, y, 100);
		CHECK_TRUE(f.model->engine().state(0).playing);
		CHECK_EQUAL(1, f.host.allocationAttempts);
	}
	f.view.padAction(17, 7, 0);
	CHECK_TRUE(f.model->engine().state(0).playing);
	f.view.padAction(17, 7, 100);
	CHECK_FALSE(f.model->engine().state(0).playing);
	for (int32_t y = 0; y < 7; ++y) {
		f.view.padAction(16, y, 100);
		f.view.padAction(17, y, 100);
		CHECK_FALSE(f.model->engine().state(0).playing);
	}
	f.view.padAction(16, 7, 0);
	CHECK_FALSE(f.model->engine().state(0).playing);
	f.view.padAction(16, 7, 100);
	CHECK_TRUE(f.model->engine().state(0).playing);
	CHECK_EQUAL(1, f.host.allocationAttempts);
}

TEST(LoopUi, PerformanceGraphicsRefreshInvalidatesOledAfterModelAndAudioStateChanges) {
	Fixture f;
	CHECK_FALSE(f.model->hasUnsavedAudio());
	CHECK(Status::Ok == f.model->ensureBound(0));
	probeOledRequests = probeRenderRequests = 0;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.renderAudio();
	CHECK_TRUE(f.model->hasUnsavedAudio());
	CHECK_EQUAL(0, probeOledRequests); // Audio rendering must not request UI work.
	f.view.graphicsRoutine();
	CHECK(probeRenderRequests > 0);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, SevenSegmentPerformanceGraphicsRefreshDoesNotRequestOled) {
	Fixture f;
	f.screen.oled = false;
	probeOledRequests = probeRenderRequests = 0;
	f.view.graphicsRoutine();
	CHECK(probeRenderRequests > 0);
	CHECK_EQUAL(0, probeOledRequests);
}

namespace {
int32_t borderX(SlotId slot) {
	return (slot % 4) * 4;
}
int32_t borderY(SlotId slot) {
	return slot < 4 ? 7 : 3;
}
void selectBorder(Fixture& f, SlotId slot) {
	f.view.padAction(borderX(slot), borderY(slot), 100);
}
void expectLastTarget(void* expected) {
	CHECK_FALSE(::view.targets.empty());
	POINTERS_EQUAL(expected, ::view.targets.back());
}
} // namespace

TEST(LoopUi, EntryAndNoSelectionUseSafeSongTarget) {
	Fixture f;
	CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());
	CHECK_EQUAL(1, ::view.targets.size());
	POINTERS_EQUAL(&f.song, ::view.targets.back());
	CHECK_TRUE(::view.forceSongGlobalEffectsTarget);
	POINTERS_EQUAL(nullptr, f.song.currentClip);
	CHECK(UIModControllableContext::SONG == f.view.getUIModControllableContext());
	CHECK_FALSE(f.view.getAffectEntire());
	CHECK_EQUAL(0, f.host.allocationAttempts);

	// A persisted song-wide mode survives entry rather than being cleared.
	f.song.affectEntire = true;
	::view.targets.clear();
	CHECK_TRUE(f.view.opened());
	CHECK_TRUE(f.song.affectEntire);
	expectLastTarget(&f.song);
	CHECK(UIModControllableContext::SONG == f.view.getUIModControllableContext());

	currentSong = nullptr;
	::view.targets.clear();
	f.view.focusRegained();
	CHECK_EQUAL(1, ::view.targets.size());
	POINTERS_EQUAL(nullptr, ::view.targets.back());
	CHECK(UIModControllableContext::NONE == f.view.getUIModControllableContext());
	CHECK_FALSE(f.view.getAffectEntire());
	CHECK_FALSE(f.view.opened());
}

TEST(LoopUi, AffectEntireLedUsesLoopStationSongModeInAllTargetStates) {
	deluge::gui::views::setAffectEntireLedState(true, UIType::LOOP_STATION, false, false, [](bool on) {
		indicator_leds::setLedState(IndicatorLED::AFFECT_ENTIRE, on);
	});
	CHECK_FALSE(indicator_leds::states[IndicatorLED::AFFECT_ENTIRE]); // selected loop

	deluge::gui::views::setAffectEntireLedState(false, UIType::LOOP_STATION, true, false, [](bool on) {
		indicator_leds::setLedState(IndicatorLED::AFFECT_ENTIRE, on);
	});
	CHECK_TRUE(indicator_leds::states[IndicatorLED::AFFECT_ENTIRE]); // song-wide FX

	deluge::gui::views::setAffectEntireLedState(false, UIType::LOOP_STATION, false, false, [](bool on) {
		indicator_leds::setLedState(IndicatorLED::AFFECT_ENTIRE, on);
	});
	CHECK_FALSE(indicator_leds::states[IndicatorLED::AFFECT_ENTIRE]); // deselected, song mode off

	// Preserve the pre-existing behavior in other clip views.
	deluge::gui::views::setAffectEntireLedState(true, UIType::AUDIO_CLIP, false, false, [](bool on) {
		indicator_leds::setLedState(IndicatorLED::AFFECT_ENTIRE, on);
	});
	CHECK_TRUE(indicator_leds::states[IndicatorLED::AFFECT_ENTIRE]);
}

TEST(LoopUi, StutterRetargetRestoresSourceRateAndConsumesRelease) {
	Fixture f;
	selectBorder(f, 0);
	ParamManagerForTimeline sourceParams;
	ParamManagerForTimeline destinationParams;
	constexpr int32_t sourceRate = 20 << 25;
	constexpr int32_t destinationRate = -(24 << 25);
	constexpr int32_t rateParam = deluge::modulation::params::UNPATCHED_STUTTER_RATE;
	sourceParams.params.params[rateParam].value = sourceRate;
	destinationParams.params.params[rateParam].value = destinationRate;
	ProbeAudioModControllable source;
	source.mode = 6;
	view.activeModControllableModelStack.modControllable = &source;
	view.activeModControllableModelStack.paramManager = &sourceParams;
	view.nextTargetModKnobMode = 2;
	view.nextTargetParamManager = &destinationParams;
	CHECK(source.mode != view.nextTargetModKnobMode);
	CHECK_EQUAL(sourceRate, sourceParams.params.getValue(rateParam));
	CHECK_EQUAL(destinationRate, destinationParams.params.getValue(rateParam));
	CHECK(Error::NONE
	      == stutterer.beginStutter(&source, &sourceParams, StutterConfig{.quantized = true}, 13, 0x40000000));
	CHECK_EQUAL(0, sourceParams.params.getValue(rateParam));
	CHECK_EQUAL(destinationRate, destinationParams.params.getValue(rateParam));
	CHECK_TRUE(stutterer.isStuttering(&source));
	currentUIMode = UI_MODE_STUTTERING;
	f.view.updateModTarget(); // Same-target refreshes must not end the active gesture.
	CHECK_TRUE(currentUIMode & UI_MODE_STUTTERING);
	CHECK_EQUAL(0, sourceParams.params.getValue(rateParam));
	CHECK_EQUAL(0, view.automationNotifications);

	CHECK(f.view.buttonAction(AFFECT_ENTIRE, true, false) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(sourceRate, sourceParams.params.getValue(rateParam));
	CHECK_EQUAL(destinationRate, destinationParams.params.getValue(rateParam));
	CHECK_FALSE(stutterer.isStuttering(&source));
	CHECK_FALSE(currentUIMode & UI_MODE_STUTTERING);
	CHECK_EQUAL(1, view.automationNotifications);
	POINTERS_EQUAL(&sourceParams, view.lastAutomationParamManager);
	CHECK_EQUAL(view.nextTargetModKnobMode, view.activeModKnobMode);
	POINTERS_EQUAL(&destinationParams, view.activeModControllableModelStack.paramManager);
	ProbeAudioModControllable destination;
	view.activeModControllableModelStack.modControllable = &destination;
	CHECK(loopStationSettingsMenu.openGlobal());
	POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
	// This lifecycle case intentionally simulates a missing/stale release: it sends
	// no physical release here, so pending ownership is retained until a fresh press
	// arrives. The complete Buttons probe covers the real path, where an owned
	// canceled release is consumed before the settings handler.
	CHECK_TRUE(targetChangeView.pendingStutterEncoderRelease_);
	CHECK_EQUAL(destinationRate, destinationParams.params.getValue(rateParam));
	loopStationSettingsMenu.close();
	POINTERS_EQUAL(&f.view, getCurrentUI());

	view.activeModControllableModelStack.paramManager = &destinationParams;
	CHECK(
	    Error::NONE
	    == stutterer.beginStutter(&destination, &destinationParams, StutterConfig{.quantized = true}, 13, 0x40000000));
	CHECK_EQUAL(0, destinationParams.params.getValue(rateParam));
	currentUIMode = UI_MODE_STUTTERING;
	// A fresh physical press after an interrupted gesture must discard a stale
	// release latch; its own release must reach the newly active stutter.
	CHECK_TRUE(targetChangeView.pendingStutterEncoderRelease_);
	CHECK_FALSE(view.consumePendingStutterEncoderRelease(1, true));
	CHECK_FALSE(view.consumePendingStutterEncoderRelease(1, false));
	Buttons::probeDispatchModEncoderButtonAction(1, true);
	Buttons::probeDispatchModEncoderButtonAction(1, false);
	CHECK_FALSE(stutterer.isStuttering(&destination));
	CHECK_EQUAL(destinationRate, destinationParams.params.getValue(rateParam));
	CHECK_EQUAL(2, view.automationNotifications); // Fresh release ends its own gesture exactly once.
}

TEST(LoopUi, EveryBorderAndEncoderSelectionTargetsItsOwnClip) {
	Fixture f;
	for (SlotId slot = 0; slot < 8; ++slot) {
		::view.targets.clear();
		selectBorder(f, slot);
		CHECK_EQUAL(slot, f.model->engine().selected());
		auto* clip = f.model->clip(slot);
		CHECK(clip);
		expectLastTarget(clip);
		POINTERS_EQUAL(clip, f.song.currentClip);
		CHECK(UIModControllableContext::CLIP == f.view.getUIModControllableContext());
	}
	selectBorder(f, 7); // Deselect the current loop.
	CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());

	::view.targets.clear();
	f.view.selectEncoderAction(1); // Invalid selection starts at 0, then moves to 1.
	CHECK_EQUAL(1, f.model->engine().selected());
	auto* encoderClip = f.model->clip(1);
	POINTERS_EQUAL(encoderClip, ::view.targets.back());
	POINTERS_EQUAL(encoderClip, f.song.currentClip);

	// Centre actions run through the real engine; if it changes selection the
	// target must follow, and otherwise the selected clip target must remain.
	::view.targets.clear();
	f.view.padAction(centres[5][0], centres[5][1], 100);
	CHECK_EQUAL(1, f.model->engine().selected());
	expectLastTarget(encoderClip);
	f.view.padAction(centres[5][0], centres[5][1], 0);
}

TEST(LoopUi, DeselectionAndFailedBindingNeverRetainUnrelatedTarget) {
	Fixture f;
	selectBorder(f, 1);
	auto* first = f.model->clip(1);
	CHECK(first);
	POINTERS_EQUAL(first, f.song.currentClip);

	selectBorder(f, 1); // Toggle the selection off.
	CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());
	::view.targets.clear();
	f.view.focusRegained();
	expectLastTarget(&f.song);
	CHECK(UIModControllableContext::SONG == f.view.getUIModControllableContext());

	::view.targets.clear();
	f.host.failAllocation = true;
	selectBorder(f, 4);
	CHECK_FALSE(f.model->clip(4));
	CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());
	CHECK_EQUAL(0, ::view.targets.size()); // A failed binding refreshes nothing.
	CHECK(UIModControllableContext::SONG == f.view.getUIModControllableContext());

	f.host.failAllocation = false;
	selectBorder(f, 1);
	POINTERS_EQUAL(first, f.song.currentClip);
	::view.targets.clear();
	f.host.failAllocation = true;
	selectBorder(f, 4);
	CHECK_EQUAL(1, f.model->engine().selected());
	CHECK_EQUAL(0, ::view.targets.size());
	POINTERS_EQUAL(first, f.song.currentClip);
	CHECK(UIModControllableContext::CLIP == f.view.getUIModControllableContext());
}

TEST(LoopUi, AffectEntirePressTogglesOnceAndSelectionRetainsMode) {
	Fixture f;
	selectBorder(f, 0);
	CHECK(f.model->clip(0));
	::view.targets.clear();

	CHECK(f.view.buttonAction(AFFECT_ENTIRE, true, false) == ActionResult::DEALT_WITH);
	CHECK_TRUE(f.song.affectEntire);
	CHECK_TRUE(f.view.getAffectEntire());
	CHECK_EQUAL(1, ::view.targets.size());
	POINTERS_EQUAL(&f.song, ::view.targets.back());
	CHECK_TRUE(::view.forceSongGlobalEffectsTarget);
	CHECK(UIModControllableContext::SONG == f.view.getUIModControllableContext());

	CHECK(f.view.buttonAction(AFFECT_ENTIRE, false, false) == ActionResult::DEALT_WITH);
	CHECK_TRUE(f.song.affectEntire);
	CHECK_EQUAL(1, ::view.targets.size()); // Releases never toggle or refresh.

	selectBorder(f, 1);
	auto* latest = f.model->clip(1);
	CHECK(latest);
	CHECK_TRUE(f.song.affectEntire);
	expectLastTarget(&f.song);
	CHECK(UIModControllableContext::SONG == f.view.getUIModControllableContext());

	CHECK(f.view.buttonAction(AFFECT_ENTIRE, true, false) == ActionResult::DEALT_WITH);
	CHECK_FALSE(f.song.affectEntire);
	expectLastTarget(latest);
	POINTERS_EQUAL(latest, f.song.currentClip);
	CHECK(UIModControllableContext::CLIP == f.view.getUIModControllableContext());

	::view.reset();
	currentUIMode = UI_MODE_HOLDING_SAVE_BUTTON;
	CHECK(f.view.buttonAction(AFFECT_ENTIRE, true, false) == ActionResult::NOT_DEALT_WITH);
	CHECK_FALSE(f.song.affectEntire);
	CHECK_EQUAL(0, ::view.targets.size());
	CHECK_EQUAL(1, ::view.calls.size());
	currentUIMode = UI_MODE_NONE;

	currentUIMode = UI_MODE_STUTTERING;
	CHECK(f.view.buttonAction(AFFECT_ENTIRE, true, false) == ActionResult::DEALT_WITH);
	CHECK_TRUE(f.song.affectEntire);
	expectLastTarget(&f.song);
	currentUIMode = UI_MODE_NONE;
}

TEST(LoopUi, FocusRegainRestoresTargetAfterMenus) {
	Fixture f;
	selectBorder(f, 0);
	CHECK(f.model->clip(0));
	CHECK(f.view.buttonAction(SELECT_ENC, true, false) == ActionResult::DEALT_WITH);
	POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());

	CHECK(Status::Ok == f.model->ensureBound(1));
	CHECK(Status::Ok == f.model->engine().dispatch({CommandType::Select, 1}, f.host.time, f.model->clock()));
	CHECK_EQUAL(1, f.model->engine().selected());
	loopStationSettingsMenu.buttonAction(BACK, true, false);
	POINTERS_EQUAL(&f.view, getCurrentUI());

	auto* restored = f.model->clip(1);
	CHECK(restored);
	expectLastTarget(restored);
	POINTERS_EQUAL(restored, f.song.currentClip);

	const auto allocations = f.host.allocationAttempts;
	::view.targets.clear();
	for (int i = 0; i < 3; ++i)
		f.view.focusRegained();
	CHECK_EQUAL(3, ::view.targets.size());
	for (auto* target : ::view.targets)
		POINTERS_EQUAL(restored, target);
	CHECK_EQUAL(allocations, f.host.allocationAttempts);

	::view.targets.clear();
	f.view.graphicsRoutine();
	CHECK_EQUAL(0, ::view.targets.size());
	CHECK_EQUAL(allocations, f.host.allocationAttempts);
}

namespace {
using Prompt = LoopStationView::HistoryPromptState;

// Model-level setup: a completed initial take, playing; and a completed overdub
// layer on top. The view shortcut under test is always driven through the real
// root handler, never by setting a boolean directly.
void modelRecord(loop_ui_test::Fixture& f, SlotId slot) {
	f.model->serviceRetirement();
	CHECK(Status::Ok == f.model->ensureBound(slot));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, slot}));
	f.renderAudio(8);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, slot}));
	CHECK_FALSE(f.model->engine().state(slot).capture);
	CHECK_TRUE(f.model->engine().state(slot).hasPhrase);
}

void modelOverdub(loop_ui_test::Fixture& f, SlotId slot) {
	// Step past the mandatory provisional double-tap window so this press opens a
	// real overdub rather than cancelling the just-recorded take.
	f.host.time += loop_station::GestureRollback::kProvisionalSamples + 1;
	probeSampleClock = f.host.time;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, slot}));
	CHECK(f.model->engine().state(slot).capture);
	f.renderAudio(16);
	// The open press set the provisional tap latch; step past it so this press
	// confirms and closes the overdub instead of cancelling it as a double-tap.
	f.host.time += loop_station::GestureRollback::kProvisionalSamples + 1;
	probeSampleClock = f.host.time;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, slot}));
	CHECK_FALSE(f.model->engine().state(slot).capture);
}

HistoryNavigationResult driveHistory(loop_ui_test::Fixture& f, RestoreId id) {
	for (unsigned i = 0; i < 5000; ++i) {
		f.model->serviceHistoryNavigation(128);
		const auto result = f.model->historyNavigationResult();
		if (result.id.generation == id.generation && !result.pending())
			return result;
	}
	return f.model->historyNavigationResult();
}

void shiftOn(loop_ui_test::Fixture& f) {
	::view.reset();
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.sharedShift(f.view, true) == ActionResult::NOT_DEALT_WITH);
	CHECK_TRUE(Buttons::shiftPressed);
}

void shiftOff(loop_ui_test::Fixture& f) {
	::view.reset();
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.sharedShift(f.view, false) == ActionResult::NOT_DEALT_WITH);
	CHECK_FALSE(Buttons::shiftPressed);
}

void armUndo(loop_ui_test::Fixture& f) {
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(Prompt::Confirm == f.view.historyPromptState_);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
}

void confirmSecondPress(loop_ui_test::Fixture& f) {
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(Prompt::Applying == f.view.historyPromptState_);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
}

bool backingIs(loop_ui_test::Fixture& f, SlotId slot, int32_t value) {
	const auto frames = f.renderTrack(slot);
	for (const auto& frame : frames)
		if (frame.l != value || frame.r != value)
			return false;
	return true;
}
} // namespace

TEST(LoopUi, UndoRequiresTwoReleasedBackPresses) {
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		Fixture f;
		modelRecord(f, slot);
		selectBorder(f, slot);
		CHECK_EQUAL(slot, f.model->engine().selected());
		const auto backing = f.renderTrack(slot);

		// The first press only arms the confirmation.
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(Prompt::Confirm == f.view.historyPromptState_);
		CHECK_EQUAL(slot, f.view.historyTarget_.slot);
		CHECK_FALSE(f.view.historyTarget_.redo);
		CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
		CHECK(f.model->engine().state(slot).hasPhrase);
		const auto afterFirst = f.renderTrack(slot);
		CHECK_EQUAL(backing.size(), afterFirst.size()); // exact audio unchanged
		for (size_t i = 0; i < backing.size(); ++i) {
			CHECK_EQUAL(backing[i].l, afterFirst[i].l);
			CHECK_EQUAL(backing[i].r, afterFirst[i].r);
		}

		// A repeated press with no release cannot confirm; the release alone does
		// not edit either.
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(Prompt::Confirm == f.view.historyPromptState_);
		CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
		CHECK(f.model->engine().state(slot).hasPhrase);
		CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
		CHECK(Prompt::Confirm == f.view.historyPromptState_);

		// The second released press submits exactly once.
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(Prompt::Applying == f.view.historyPromptState_);
		CHECK(f.view.historyId_.valid());
		CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
		const auto id = f.view.historyId_;
		CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, id).state));
		CHECK_FALSE(f.model->engine().state(slot).hasPhrase);
		f.view.graphicsRoutine();
		CHECK(Prompt::Result == f.view.historyPromptState_);
	}

	// A third press starts a new confirmation rather than removing another layer.
	{
		Fixture f;
		modelRecord(f, 0);
		modelOverdub(f, 0);
		selectBorder(f, 0);
		CHECK_TRUE(f.model->canUndo(0));
		armUndo(f);
		confirmSecondPress(f);
		CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, f.view.historyId_).state));
		CHECK_TRUE(f.model->engine().state(0).hasPhrase); // the initial layer remains
		const auto frames = f.model->engine().state(0).phraseFrames;
		f.view.graphicsRoutine();
		const auto prior = f.model->historyNavigationResult();
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
		CHECK(Prompt::Confirm == f.view.historyPromptState_);
		CHECK_TRUE(f.model->engine().state(0).hasPhrase);
		CHECK_EQUAL(frames, f.model->engine().state(0).phraseFrames);
		CHECK_EQUAL(prior.id.generation, f.model->historyNavigationResult().id.generation);
		CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
		confirmSecondPress(f);
		CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, f.view.historyId_).state));
		CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	}

	// AFFECT ENTIRE never retargets the shortcut; the selected track still undoes.
	{
		Fixture f;
		modelRecord(f, 2);
		selectBorder(f, 2);
		CHECK(f.view.buttonAction(AFFECT_ENTIRE, true, false) == ActionResult::DEALT_WITH);
		CHECK_TRUE(f.song.affectEntire);
		armUndo(f);
		CHECK_EQUAL(2, f.view.historyTarget_.slot);
		confirmSecondPress(f);
		CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, f.view.historyId_).state));
		CHECK_FALSE(f.model->engine().state(2).hasPhrase);
	}
}

TEST(LoopUi, RedoRequiresTwoReleasedShiftBackPresses) {
	Fixture f;
	modelRecord(f, 0);
	selectBorder(f, 0);
	armUndo(f);
	confirmSecondPress(f);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, f.view.historyId_).state));
	f.view.graphicsRoutine();
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	CHECK_TRUE(f.model->canRedo(0));

	shiftOn(f);
	// The first SHIFT+BACK arms Redo without restoring anything.
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(Prompt::Confirm == f.view.historyPromptState_);
	CHECK_TRUE(f.view.historyTarget_.redo);
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	// The second SHIFT+BACK confirms.
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(Prompt::Applying == f.view.historyPromptState_);
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	const auto result = driveHistory(f, f.view.historyId_);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(result.state));
	CHECK_TRUE(result.redo);
	CHECK_TRUE(f.model->engine().state(0).hasPhrase);
	CHECK_FALSE(f.model->engine().state(0).playing); // restored stopped
	shiftOff(f);
	CHECK_TRUE(backingIs(f, 0, 100));
}

TEST(LoopUi, OppositeShortcutRearmsRatherThanConfirms) {
	Fixture f;
	modelRecord(f, 0);
	modelOverdub(f, 0);
	// Undo the overdub directly on the model: the initial layer remains and the
	// removed overdub branch is redoable.
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, id).state));
	CHECK_TRUE(f.model->canUndo(0));
	CHECK_TRUE(f.model->canRedo(0));
	selectBorder(f, 0);
	const auto prior = f.model->historyNavigationResult();

	armUndo(f);
	CHECK_FALSE(f.view.historyTarget_.redo);
	// The opposite shortcut re-arms Redo instead of confirming the pending Undo.
	shiftOn(f);
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(Prompt::Confirm == f.view.historyPromptState_);
	CHECK_TRUE(f.view.historyTarget_.redo);
	CHECK_EQUAL(prior.id.generation, f.model->historyNavigationResult().id.generation);
	CHECK_TRUE(f.model->engine().state(0).hasPhrase);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	// A second SHIFT+BACK confirms the Redo.
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(Prompt::Applying == f.view.historyPromptState_);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	const auto redone = driveHistory(f, f.view.historyId_);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(redone.state));
	CHECK_TRUE(redone.redo);
	shiftOff(f);
	// The Redo restored the thread: a layer is again available to undo and the
	// forward branch is consumed.
	CHECK_TRUE(f.model->engine().state(0).hasPhrase);
	CHECK_TRUE(f.model->canUndo(0));
	CHECK_FALSE(f.model->canRedo(0));
}

TEST(LoopUi, ShiftEdgesAlonePreserveRedoPrompt) {
	Fixture f;
	modelRecord(f, 0);
	selectBorder(f, 0);
	armUndo(f);
	confirmSecondPress(f);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, f.view.historyId_).state));
	f.view.graphicsRoutine();
	const auto prior = f.model->historyNavigationResult();
	shiftOn(f);
	armUndo(f); // reuse the released press pairing: arms Redo because SHIFT is held
	CHECK_TRUE(f.view.historyTarget_.redo);
	// SHIFT edges alone never cancel or confirm the prompt.
	shiftOff(f);
	CHECK(Prompt::Confirm == f.view.historyPromptState_);
	CHECK_TRUE(f.view.historyTarget_.redo);
	shiftOn(f);
	CHECK(Prompt::Confirm == f.view.historyPromptState_);
	CHECK_TRUE(f.view.historyTarget_.redo);
	CHECK_EQUAL(prior.id.generation, f.model->historyNavigationResult().id.generation);
	confirmSecondPress(f);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, f.view.historyId_).state));
	shiftOff(f);
	CHECK_TRUE(f.model->engine().state(0).hasPhrase);
}

TEST(LoopUi, UnrelatedActionsAndSelectionCancelPrompt) {
	auto prepare = [](Fixture& f) {
		modelRecord(f, 0);
		modelOverdub(f, 0);
		selectBorder(f, 0);
		armUndo(f);
		CHECK(Prompt::Confirm == f.view.historyPromptState_);
	};
	{
		Fixture f;
		prepare(f);
		f.view.padAction(4, 7, 100); // border selection change
		CHECK(Prompt::None == f.view.historyPromptState_);
	}
	{
		Fixture f;
		prepare(f);
		f.view.selectEncoderAction(1);
		CHECK(Prompt::None == f.view.historyPromptState_);
	}
	{
		Fixture f;
		prepare(f);
		f.view.modEncoderAction(0, 3);
		CHECK(Prompt::None == f.view.historyPromptState_);
		CHECK_EQUAL(1, ::view.modEncoderActionCalls); // existing forwarding preserved
	}
	{
		Fixture f;
		prepare(f);
		f.view.modButtonAction(0, true);
		CHECK(Prompt::None == f.view.historyPromptState_);
		CHECK_EQUAL(1, ::view.modButtonActionCalls);
	}
	{
		Fixture f;
		prepare(f);
		f.view.modEncoderButtonAction(1, true);
		CHECK(Prompt::None == f.view.historyPromptState_);
		CHECK_EQUAL(1, ::view.modEncoderButtonCalls);
	}
	{
		Fixture f;
		prepare(f);
		::view.reset();
		::view.result = ActionResult::NOT_DEALT_WITH;
		f.view.buttonAction(TAP_TEMPO, true, false);
		CHECK(Prompt::None == f.view.historyPromptState_);
		CHECK_EQUAL(1, ::view.calls.size());
	}
	{
		Fixture f;
		prepare(f);
		f.view.buttonAction(AFFECT_ENTIRE, true, false);
		CHECK(Prompt::None == f.view.historyPromptState_);
	}
	// A release edge alone never cancels a prompt.
	{
		Fixture f;
		prepare(f);
		::view.reset();
		::view.result = ActionResult::NOT_DEALT_WITH;
		f.view.buttonAction(TAP_TEMPO, false, false);
		CHECK(Prompt::Confirm == f.view.historyPromptState_);
	}
	// A SHIFT edge alone never cancels a prompt.
	{
		Fixture f;
		prepare(f);
		shiftOn(f);
		CHECK(Prompt::Confirm == f.view.historyPromptState_);
		shiftOff(f);
		CHECK(Prompt::Confirm == f.view.historyPromptState_);
	}
}

TEST(LoopUi, SharedMenusOwnBack) {
	{
		Fixture f;
		modelRecord(f, 0);
		modelOverdub(f, 0);
		selectBorder(f, 0);
		armUndo(f);
		// Opening a shared menu is an outgoing action: the prompt is dropped.
		CHECK(f.view.buttonAction(SELECT_ENC, true, false) == ActionResult::DEALT_WITH);
		POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
		CHECK(Prompt::None == f.view.historyPromptState_);
		// The menu keeps BACK navigation.
		loopStationSettingsMenu.buttonAction(BACK, true, false);
		POINTERS_EQUAL(&f.view, getCurrentUI());
		CHECK(Prompt::None == f.view.historyPromptState_);
	}
	{
		// A shared exclusive mode owns BACK; it never arms or confirms.
		Fixture f;
		modelRecord(f, 0);
		selectBorder(f, 0);
		currentUIMode = UI_MODE_HOLDING_SAVE_BUTTON;
		::view.reset();
		::view.result = ActionResult::NOT_DEALT_WITH;
		CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::NOT_DEALT_WITH);
		CHECK_EQUAL(1, ::view.calls.size());
		CHECK(Prompt::None == f.view.historyPromptState_);
		CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
		currentUIMode = UI_MODE_NONE;
	}
}

TEST(LoopUi, CardDeferralDoesNotCountAsAConfirmation) {
	Fixture f;
	modelRecord(f, 0);
	selectBorder(f, 0);
	// A deferred card-routine press is not counted at all.
	CHECK(f.view.buttonAction(BACK, true, true) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK(Prompt::None == f.view.historyPromptState_);
	// Its retry arms exactly once.
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(Prompt::Confirm == f.view.historyPromptState_);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	// A deferred press cannot stand in for the confirming second press.
	CHECK(f.view.buttonAction(BACK, true, true) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK(Prompt::Confirm == f.view.historyPromptState_);
	CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
	CHECK(f.model->engine().state(0).hasPhrase);
	// Only a real released second press confirms.
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(Prompt::Applying == f.view.historyPromptState_);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, f.view.historyId_).state));
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
}

TEST(LoopUi, NoSelectionNeverDelegatesGlobalUndo) {
	Fixture f;
	CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());
	::view.reset();
	::view.result = ActionResult::NOT_DEALT_WITH;
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(0, ::view.calls.size()); // never falls through to a global Undo
	CHECK(Prompt::Result == f.view.historyPromptState_);
	CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(Prompt::Result == f.view.historyPromptState_);
}

TEST(LoopUi, PendingCaptureConfirmationTracksSessionIdentity) {
	// More frames inside one active overdub session do not invalidate the prompt;
	// the same session still confirms.
	{
		Fixture f;
		modelRecord(f, 0);
		selectBorder(f, 0);
		CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
		CHECK(f.model->engine().state(0).capture);
		armUndo(f);
		const auto epoch = f.view.historyTarget_.historyEpoch;
		f.renderAudio(16);
		f.view.graphicsRoutine();
		CHECK(Prompt::Confirm == f.view.historyPromptState_);
		CHECK_EQUAL(epoch, f.view.historyTarget_.historyEpoch);
		confirmSecondPress(f);
		CHECK_FALSE(f.model->engine().state(0).capture);
		CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, f.view.historyId_).state));
		CHECK_TRUE(f.model->engine().state(0).hasPhrase);
		CHECK_TRUE(f.model->engine().state(0).playing);
	}
	// Ending/replacing the capture session invalidates the prompt.
	{
		Fixture f;
		modelRecord(f, 0);
		selectBorder(f, 0);
		CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
		armUndo(f);
		f.renderAudio(16);
		CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0})); // close the session
		f.view.graphicsRoutine();
		CHECK(Prompt::None == f.view.historyPromptState_);
		CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
	}
}

TEST(LoopUi, AutomaticCaptureCompletionInvalidatesPendingConfirmation) {
	Fixture f;
	selectBorder(f, 0); // bind/select an empty track
	auto settings = f.model->engine().settings();
	settings.tracks[0].recordDurationBars = 1;
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	f.model->updateClock(f.host.time, 1000u << 16, true, 0);         // a short bar keeps the take brief
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0})); // active initial take
	CHECK(f.model->engine().state(0).capture);
	armUndo(f);
	for (unsigned i = 0; i < 400 && f.model->engine().state(0).capture; ++i)
		f.renderAudio(128);
	CHECK_FALSE(f.model->engine().state(0).capture); // completed on its own
	CHECK(f.model->engine().state(0).hasPhrase);
	// Automatic completion is a different session: the pending prompt cancels.
	f.view.graphicsRoutine();
	CHECK(Prompt::None == f.view.historyPromptState_);
}

TEST(LoopUi, ApplyingPressesAreConsumedWithoutQueuing) {
	Fixture f;
	modelRecord(f, 0);
	modelOverdub(f, 0);
	selectBorder(f, 0);
	armUndo(f);
	confirmSecondPress(f);
	const auto id = f.view.historyId_;
	CHECK(Prompt::Applying == f.view.historyPromptState_);
	CHECK(f.model->engine().state(0).hasPhrase);
	// Extra released presses neither queue a second request nor apply anything.
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(BACK, true, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(BACK, false, false) == ActionResult::DEALT_WITH);
	CHECK(Prompt::Applying == f.view.historyPromptState_);
	CHECK_EQUAL(id.generation, f.model->historyNavigationResult().id.generation);
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(f.model->historyNavigationResult().state));
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(driveHistory(f, id).state));
	CHECK_TRUE(f.model->engine().state(0).hasPhrase);
}

TEST(LoopUi, TransportOrModelChangeCancelsPrompt) {
	{
		Fixture f;
		modelRecord(f, 0);
		modelOverdub(f, 0);
		selectBorder(f, 0);
		armUndo(f);
		// A transport running/stopped transition invalidates the armed target.
		f.model->updateClock(f.host.time, 120u << 16, true, 0);
		f.view.graphicsRoutine();
		CHECK(Prompt::None == f.view.historyPromptState_);
		CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
	}
	{
		Fixture f;
		modelRecord(f, 0);
		modelOverdub(f, 0);
		selectBorder(f, 0);
		armUndo(f);
		// Rebinding replaces the slot identity; the saved target is dead.
		CHECK(Status::Ok == f.model->unbind(0));
		f.view.graphicsRoutine();
		CHECK(Prompt::None == f.view.historyPromptState_);
		CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
	}
}
