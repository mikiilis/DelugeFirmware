#include "deluge/loop_station/persistence.h"
#include "fixture.h"

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
} // namespace

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

TEST(LoopUi, EncoderPreparesIntendedSlotAndRecordUsesThatSelection) {
	Fixture f;
	f.view.selectEncoderAction(2);
	f.onlyBound(2);
	CHECK_EQUAL(2, f.model->engine().selected());
	f.view.buttonAction(RECORD, true, false);
	CHECK_TRUE(f.model->engine().state(2).capture);
	CHECK_EQUAL(1, f.host.allocationAttempts);
	f.view.selectEncoderAction(-1);
	CHECK_EQUAL(1, f.model->engine().selected());
	CHECK_TRUE(f.model->clip(1));
	CHECK_EQUAL(2, f.host.allocationAttempts);
}

TEST(LoopUi, RecordPreparesAnUnboundSelectedTrack) {
	Fixture f;
	// Engine selection may exist independently of a firmware clip binding.
	CHECK(Status::Ok == f.model->engine().dispatch({CommandType::Select, 3}, f.host.time, f.model->clock()));
	f.view.buttonAction(RECORD, true, false);
	f.onlyBound(3);
	CHECK_TRUE(f.model->engine().state(3).capture);
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
	STRCMP_EQUAL("Track unavailable", f.screen.popups.back().c_str());
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

TEST(LoopUi, OnlyShiftPropagatesAndCardRoutineButtonDeferralIsPreserved) {
	Fixture f;
	CHECK(f.view.buttonAction(SYNTH, true, false) == ActionResult::DEALT_WITH);
	CHECK(loopStationSettingsMenu.buttonAction(SYNTH, true, false) == ActionResult::DEALT_WITH);
	CHECK(f.view.buttonAction(RECORD, true, true) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK(loopStationSettingsMenu.buttonAction(SELECT_ENC, true, true) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK_EQUAL(0, f.host.allocationAttempts);
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
	STRCMP_EQUAL("Loop Station Global", f.screen.text.c_str());
	STRCMP_EQUAL("PLAY DUB", f.screen.popups.back().c_str());
	f.screen.text.clear();
	const auto before = f.screen.popups.size();
	loopStationSettingsMenu.selectEncoderAction(1);
	CHECK_EQUAL(1, loopStationSettingsMenu.row_);
	STRCMP_EQUAL("Loop Station Global", f.screen.text.c_str());
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
	STRCMP_EQUAL("Loop Station Track", f.screen.text.c_str());
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
	STRCMP_EQUAL("Loop Station Track", f.screen.text.c_str());
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

TEST(LoopUi, PerformanceRecordCaptureInvalidatesOled) {
	Fixture f;
	f.view.selectEncoderAction(2);
	probeOledRequests = 0;
	f.view.buttonAction(RECORD, true, false);
	CHECK_TRUE(f.model->engine().state(2).capture);
	CHECK(probeOledRequests > 0);
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

TEST(LoopUi, PerformancePlayButtonInvalidatesOled) {
	Fixture f;
	f.recordPhrase();
	f.view.padAction(17, 7, 100);
	probeOledRequests = 0;
	f.view.buttonAction(PLAY, true, false);
	CHECK_TRUE(f.model->engine().state(0).playing);
	CHECK(probeOledRequests > 0);
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
