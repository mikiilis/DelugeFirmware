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

TEST(LoopView, PhysicalRowsMapToHtmlTrackOrder) {
	constexpr int32_t origins[8][2] = {{0, 7}, {4, 7}, {8, 7}, {12, 7}, {0, 3}, {4, 3}, {8, 3}, {12, 3}};
	constexpr int32_t centres[4][2] = {{1, -1}, {2, -1}, {1, -2}, {2, -2}};
	for (SlotId slot = 0; slot < 8; ++slot) {
		CHECK_EQUAL(slot, padAddress(origins[slot][0], origins[slot][1]).slot);
		for (const auto& centre : centres) {
			const auto pad = padAddress(origins[slot][0] + centre[0], origins[slot][1] + centre[1]);
			CHECK_TRUE(pad.valid);
			CHECK_EQUAL(slot, pad.slot);
			CHECK(PadRegion::Centre == pad.region);
		}
	}
	CHECK_EQUAL(0, padAddress(0, 7).slot);
	CHECK_EQUAL(4, padAddress(0, 3).slot);
	CHECK_EQUAL(7, padAddress(12, 0).slot);
}

TEST(LoopView, EveryTileRingStartsTopLeftAndRunsClockwise) {
	constexpr int32_t origins[8][2] = {{0, 7}, {4, 7}, {8, 7}, {12, 7}, {0, 3}, {4, 3}, {8, 3}, {12, 3}};
	constexpr int32_t clockwiseOffsets[12][2] = {{0, 0},  {1, 0},  {2, 0},  {3, 0},  {3, -1}, {3, -2},
	                                             {3, -3}, {2, -3}, {1, -3}, {0, -3}, {0, -2}, {0, -1}};
	for (SlotId slot = 0; slot < 8; ++slot) {
		for (uint8_t i = 0; i < 12; ++i) {
			const auto pad =
			    padAddress(origins[slot][0] + clockwiseOffsets[i][0], origins[slot][1] + clockwiseOffsets[i][1]);
			CHECK_TRUE(pad.valid);
			CHECK_EQUAL(slot, pad.slot);
			CHECK(PadRegion::Border == pad.region);
			CHECK_EQUAL(i, pad.borderIndex);
		}
	}
}

TEST(LoopView, InvalidPhysicalCoordinatesStayInvalid) {
	constexpr int32_t invalid[][2] = {{-1, 7},
	                                  {0, 8},
	                                  {16, 0},
	                                  {0, -1},
	                                  {-1, -1},
	                                  {16, 8},
	                                  {std::numeric_limits<int32_t>::min(), 0},
	                                  {0, std::numeric_limits<int32_t>::max()}};
	for (const auto& coordinate : invalid) {
		const auto pad = padAddress(coordinate[0], coordinate[1]);
		CHECK_FALSE(pad.valid);
		CHECK_EQUAL(kInvalidSlot, pad.slot);
		CHECK(PadRegion::Invalid == pad.region);
	}
}
