#include "fixture.h"

using namespace loop_station;
using namespace deluge::hid::button;
using deluge::gui::menu_item::loop_station::loopStationSettingsMenu;
using loop_ui_test::Fixture;

TEST_GROUP(LoopUi){};

// Missing preparation must fail through real SongModel bindings and Engine state,
// not merely through a call count on a stand-in model.
TEST(LoopUi, FreshCentrePressBindsOnlyItsSlotAndStartsCapture) {
	Fixture f;
	CHECK_EQUAL(0, f.host.allocationAttempts);
	CHECK(f.view.padAction(9, 5, 100) == ActionResult::DEALT_WITH);
	f.onlyBound(6);
	CHECK_TRUE(f.model->engine().state(6).capture);
}

TEST(LoopUi, BorderPreparesAndTogglesSelectionWithoutCapture) {
	Fixture f;
	f.view.padAction(12, 4, 100);
	f.onlyBound(7);
	CHECK_EQUAL(7, f.model->engine().selected());
	CHECK_FALSE(f.model->engine().state(7).capture);
	f.view.padAction(12, 4, 0);
	CHECK_EQUAL(7, f.model->engine().selected());
	f.view.padAction(12, 4, 100);
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
	f.view.padAction(1, 1, 100);
	f.onlyBound(0);
	auto* clip = f.model->clip(0);
	f.view.padAction(1, 1, 0);
	f.view.padAction(1, 1, 100);
	POINTERS_EQUAL(clip, f.model->clip(0));
	CHECK_EQUAL(1, f.host.allocationAttempts);
}

TEST(LoopUi, UnboundReleaseAndGroupControlsAllocateNothing) {
	Fixture f;
	f.view.padAction(5, 1, 0);
	f.view.padAction(4, 0, 0);
	f.view.padAction(16, 0, 100);
	f.view.padAction(17, 0, 100);
	f.view.buttonAction(PLAY, true, false);
	f.view.buttonAction(RECORD, false, false);
	CHECK_EQUAL(0, f.host.allocationAttempts);
	CHECK_EQUAL(0, f.host.published.size());
}

TEST(LoopUi, PreparationFailureShowsPopupAndCentreRetryCaptures) {
	Fixture f;
	f.host.failAllocation = true;
	f.view.padAction(5, 5, 100);
	CHECK_EQUAL(1, f.host.allocationAttempts);
	CHECK_FALSE(f.model->clip(5));
	CHECK_FALSE(f.model->engine().state(5).capture);
	CHECK_EQUAL(1, f.screen.popups.size());
	STRCMP_EQUAL("Track unavailable", f.screen.popups.back().c_str());
	f.host.failAllocation = false;
	f.view.padAction(5, 5, 100);
	f.onlyBound(5);
	CHECK_TRUE(f.model->engine().state(5).capture);
}

TEST(LoopUi, FailedSelectionDoesNotChangeSelectionAndRetries) {
	Fixture f;
	f.host.failAllocation = true;
	f.view.padAction(4, 0, 100);
	CHECK_EQUAL(1, f.screen.popups.size());
	CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());
	f.host.failAllocation = false;
	f.view.padAction(4, 0, 100);
	f.onlyBound(1);
	CHECK_EQUAL(1, f.model->engine().selected());
}

TEST(LoopUi, LockedCentreDefersAllocationThenRetryCaptures) {
	Fixture f;
	sdRoutineLock = true;
	CHECK(f.view.padAction(1, 1, 100) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK_EQUAL(0, f.host.allocationAttempts);
	sdRoutineLock = false;
	f.view.padAction(1, 1, 100);
	f.onlyBound(0);
	CHECK_TRUE(f.model->engine().state(0).capture);
}

TEST(LoopUi, LockedBorderDefersSelectionThenRetryPrepares) {
	Fixture f;
	sdRoutineLock = true;
	CHECK(f.view.padAction(8, 4, 100) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK_EQUAL(0, f.host.allocationAttempts);
	CHECK_EQUAL(kInvalidSlot, f.model->engine().selected());
	sdRoutineLock = false;
	f.view.padAction(8, 4, 100);
	f.onlyBound(6);
	CHECK_EQUAL(6, f.model->engine().selected());
}

TEST(LoopUi, LockedShiftCentreDefersMenuAndAllocationThenRetryOpensTrack) {
	Fixture f;
	Buttons::shiftPressed = true;
	sdRoutineLock = true;
	CHECK(f.view.padAction(13, 1, 100) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE);
	CHECK_EQUAL(0, f.host.allocationAttempts);
	POINTERS_EQUAL(&f.view, getCurrentUI());
	sdRoutineLock = false;
	f.view.padAction(13, 1, 100);
	f.onlyBound(3);
	POINTERS_EQUAL(&loopStationSettingsMenu, getCurrentUI());
	CHECK_EQUAL(3, loopStationSettingsMenu.slot_);
	CHECK_FALSE(f.model->engine().state(3).capture);
}

TEST(LoopUi, LockedUnboundReleaseDoesNotAllocateOrDefer) {
	Fixture f;
	sdRoutineLock = true;
	CHECK(f.view.padAction(1, 1, 0) == ActionResult::DEALT_WITH);
	CHECK_EQUAL(0, f.host.allocationAttempts);
}

TEST(LoopUi, PerformanceShiftEdgesReachSharedProcessingAndOpenTrackSettings) {
	Fixture f;
	CHECK(f.sharedShift(f.view, true) == ActionResult::NOT_DEALT_WITH);
	CHECK_TRUE(Buttons::shiftPressed);
	f.view.padAction(5, 1, 100);
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
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
	CHECK_TRUE(loopStationSettingsMenu.editing_);
	CHECK_EQUAL(0, loopStationSettingsMenu.row_);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, SelectClickLeavesEditingAndInvalidatesOled) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openGlobal());
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
	probeOledRequests = 0;
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
	CHECK_FALSE(loopStationSettingsMenu.editing_);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, EditingSelectTurnChangesValueAndInvalidatesOled) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openTrack(2));
	CHECK(Source::Left == f.model->engine().settings().tracks[2].source);
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
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
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
	CHECK_TRUE(loopStationSettingsMenu.choosingCopyTarget_);
	CHECK_EQUAL(3, loopStationSettingsMenu.copyTarget_);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, CopyTargetMovementInvalidatesOled) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openTrack(2));
	loopStationSettingsMenu.selectEncoderAction(6);
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
	probeOledRequests = 0;
	loopStationSettingsMenu.selectEncoderAction(1);
	CHECK_TRUE(loopStationSettingsMenu.choosingCopyTarget_);
	CHECK_EQUAL(4, loopStationSettingsMenu.copyTarget_);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, CopyConfirmationCopiesSettingsAndInvalidatesOled) {
	Fixture f;
	CHECK_TRUE(loopStationSettingsMenu.openTrack(2));
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
	loopStationSettingsMenu.selectEncoderAction(1);
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
	loopStationSettingsMenu.selectEncoderAction(6);
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
	probeOledRequests = 0;
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
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
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
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
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
	CHECK_TRUE(loopStationSettingsMenu.choosingCopyTarget_);
	STRCMP_EQUAL("TO TR 4", f.screen.popups.back().c_str());
	loopStationSettingsMenu.selectEncoderAction(1);
	CHECK_EQUAL(4, loopStationSettingsMenu.copyTarget_);
	STRCMP_EQUAL("TO TR 5", f.screen.popups.back().c_str());
	f.screen.text.clear();
	const auto before = f.screen.popups.size();
	loopStationSettingsMenu.buttonAction(SELECT_ENC, true, false);
	CHECK_FALSE(loopStationSettingsMenu.choosingCopyTarget_);
	CHECK_TRUE(f.model->clip(4));
	STRCMP_EQUAL("Copied", f.screen.popups[before].c_str());
	STRCMP_EQUAL("TO TR 5", f.screen.popups.back().c_str());
	STRCMP_EQUAL("Loop Station Track", f.screen.text.c_str());
	CHECK_EQUAL(0, probeOledRequests);
}

TEST(LoopUi, PerformanceCentreCaptureInvalidatesOled) {
	Fixture f;
	probeOledRequests = 0;
	f.view.padAction(1, 1, 100);
	CHECK_TRUE(f.model->engine().state(0).capture);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, PerformanceBorderSelectionInvalidatesOled) {
	Fixture f;
	probeOledRequests = 0;
	f.view.padAction(4, 0, 100);
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
	f.view.padAction(17, 0, 100);
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, PerformanceGroupStartInvalidatesOled) {
	Fixture f;
	f.recordPhrase();
	f.view.padAction(17, 0, 100);
	probeOledRequests = 0;
	f.view.padAction(16, 0, 100);
	CHECK_TRUE(f.model->engine().state(0).playing);
	CHECK(probeOledRequests > 0);
}

TEST(LoopUi, PerformancePlayButtonInvalidatesOled) {
	Fixture f;
	f.recordPhrase();
	f.view.padAction(17, 0, 100);
	probeOledRequests = 0;
	f.view.buttonAction(PLAY, true, false);
	CHECK_TRUE(f.model->engine().state(0).playing);
	CHECK(probeOledRequests > 0);
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
