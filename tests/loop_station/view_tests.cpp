#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/geometry.h"
#include "deluge/loop_station/view_state.h"
#include <limits>

using namespace loop_station;

TEST_GROUP(LoopView){};

TEST(LoopView, FilledRingUsesSamplePhaseAndWideArithmetic) {
	CHECK_EQUAL(0, filledBorderPads(0, 0));
	CHECK_EQUAL(1, filledBorderPads(0, 120));
	CHECK_EQUAL(1, filledBorderPads(9, 120));
	CHECK_EQUAL(2, filledBorderPads(10, 120));
	CHECK_EQUAL(7, filledBorderPads(60, 120));
	CHECK_EQUAL(12, filledBorderPads(119, 120));
	CHECK_EQUAL(1, filledBorderPads(120, 120));
	CHECK_EQUAL(1, filledBorderPads(std::numeric_limits<FrameIndex>::max(), std::numeric_limits<FrameIndex>::max()));
}

TEST(LoopView, CrossScreenEntryRequiresIdleUnshiftedEnabledSongRoot) {
	CHECK_EQUAL(static_cast<int>(EntryDecision::Enter),
	            static_cast<int>(crossScreenEntryDecision(true, true, false, false, true)));
	CHECK_EQUAL(static_cast<int>(EntryDecision::FeatureDisabled),
	            static_cast<int>(crossScreenEntryDecision(true, true, false, false, false)));
	CHECK_EQUAL(static_cast<int>(EntryDecision::ArrangementActive),
	            static_cast<int>(crossScreenEntryDecision(true, true, false, true, true)));
	CHECK_EQUAL(static_cast<int>(EntryDecision::Ignore),
	            static_cast<int>(crossScreenEntryDecision(true, false, false, false, true)));
	CHECK_EQUAL(static_cast<int>(EntryDecision::Ignore),
	            static_cast<int>(crossScreenEntryDecision(true, true, true, false, true)));
	CHECK_EQUAL(static_cast<int>(EntryDecision::Ignore),
	            static_cast<int>(crossScreenEntryDecision(false, true, false, false, true)));
}

TEST(LoopView, ClearHoldSweepsClockwiseForTheExactTwoSecondHold) {
	constexpr SampleClock duration = 2 * 44100;
	CHECK_EQUAL(0, clearHoldSweepPads(0, duration));
	CHECK_EQUAL(2, clearHoldSweepPads(duration / 6, duration));
	CHECK_EQUAL(6, clearHoldSweepPads(duration / 2, duration));
	CHECK_EQUAL(12, clearHoldSweepPads(duration, duration));
	CHECK_EQUAL(12, clearHoldSweepPads(duration + 1, duration));
	CHECK_EQUAL(0, clearHoldSweepPads(1, 0));
}

TEST(LoopView, PendingAndErrorPulseUsesSampleClock) {
	CHECK_FALSE(visualPulseOn(0));
	CHECK_TRUE(visualPulseOn(11025));
	CHECK_FALSE(visualPulseOn(22050));
	CHECK_FALSE(visualPulseOn(123, 0));
}

TEST(LoopView, PendingActionShowsItsRecordActionDependentDestination) {
	CHECK_EQUAL(static_cast<int>(PendingDestination::Capture),
	            static_cast<int>(pendingDestination(PendingAction::Initial, RecordAction::REC_PLAY_DUB)));
	CHECK_EQUAL(static_cast<int>(PendingDestination::Play),
	            static_cast<int>(pendingDestination(PendingAction::Start, RecordAction::REC_DUB_PLAY)));
	CHECK_EQUAL(static_cast<int>(PendingDestination::Play),
	            static_cast<int>(pendingDestination(PendingAction::FinishInitial, RecordAction::REC_PLAY_DUB)));
	CHECK_EQUAL(static_cast<int>(PendingDestination::Overdub),
	            static_cast<int>(pendingDestination(PendingAction::FinishInitial, RecordAction::REC_DUB_PLAY)));
	CHECK_EQUAL(static_cast<int>(PendingDestination::Stop),
	            static_cast<int>(pendingDestination(PendingAction::Stop, RecordAction::REC_PLAY_DUB)));
	CHECK_EQUAL(static_cast<int>(PendingDestination::Play),
	            static_cast<int>(pendingDestination(PendingAction::ExitInitial, RecordAction::REC_PLAY_DUB)));
	CHECK_EQUAL(static_cast<int>(PendingDestination::Stop),
	            static_cast<int>(pendingDestination(PendingAction::StopInitial, RecordAction::REC_PLAY_DUB)));
	CHECK_EQUAL(static_cast<int>(PendingDestination::Delete),
	            static_cast<int>(pendingDestination(PendingAction::Clear, RecordAction::REC_PLAY_DUB)));
	CHECK_EQUAL(static_cast<int>(PendingDestination::None),
	            static_cast<int>(pendingDestination(PendingAction::None, RecordAction::REC_PLAY_DUB)));
}

TEST(LoopView, PadsMapToEightTilesAndClockwiseBorder) {
	const auto topLeft = padAddress(0, 0);
	CHECK_TRUE(topLeft.valid);
	CHECK_EQUAL(0, topLeft.slot);
	CHECK_EQUAL(static_cast<int>(PadRegion::Border), static_cast<int>(topLeft.region));
	CHECK_EQUAL(0, topLeft.borderIndex);

	const auto firstCentre = padAddress(1, 1);
	CHECK_EQUAL(0, firstCentre.slot);
	CHECK_EQUAL(static_cast<int>(PadRegion::Centre), static_cast<int>(firstCentre.region));
	CHECK_EQUAL(1, padAddress(4, 0).slot);
	CHECK_EQUAL(3, padAddress(12, 0).slot);
	CHECK_EQUAL(4, padAddress(0, 4).slot);
	CHECK_EQUAL(7, padAddress(12, 4).slot);
	CHECK_EQUAL(1, padAddress(1, 0).borderIndex);
	CHECK_EQUAL(4, padAddress(3, 1).borderIndex);
	CHECK_EQUAL(7, padAddress(2, 3).borderIndex);
	CHECK_EQUAL(10, padAddress(0, 2).borderIndex);
	CHECK_FALSE(padAddress(16, 0).valid);
	CHECK_FALSE(padAddress(0, 8).valid);
	const int32_t clockwise[12][2] = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {3, 1}, {3, 2},
	                                  {3, 3}, {2, 3}, {1, 3}, {0, 3}, {0, 2}, {0, 1}};
	for (uint8_t i = 0; i < 12; ++i)
		CHECK_EQUAL(i, padAddress(clockwise[i][0], clockwise[i][1]).borderIndex);
}
