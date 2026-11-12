#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/config.h"
#include "deluge/loop_station/types.h"

using namespace loop_station;

TEST_GROUP(LoopSettings){};

TEST(LoopSettings, memoryProfilesReserveEightRoundedPhrases) {
	CHECK_EQUAL(4096, kPageBytes);
	CHECK_EQUAL(33554432, activeAudioBytes(MemoryProfile::Preferred));
	CHECK_EQUAL(28246016, activeAudioBytes(MemoryProfile::TenSecond));
	CHECK_EQUAL(8 * 862 * 4096, activeAudioBytes(MemoryProfile::TenSecond));
	CHECK_EQUAL(44100, kSampleRate);
	CHECK_EQUAL(8, sizeof(Frame));
	CHECK_EQUAL(static_cast<int>(MemoryProfile::Preferred), static_cast<int>(kConfiguredMemoryProfile));
}

TEST(LoopSettings, defaultsUseSyncedSixteenthAndIndependentActions) {
	auto settings = defaultSettings();
	CHECK_EQUAL(static_cast<int>(RecordAction::REC_PLAY_DUB), static_cast<int>(settings.recordAction));
	CHECK_EQUAL(static_cast<int>(ActionMode::Synced), static_cast<int>(settings.startAction.mode));
	CHECK_EQUAL(static_cast<int>(ActionMode::Synced), static_cast<int>(settings.stopAction.mode));
	CHECK_EQUAL(8, static_cast<int>(settings.startAction.interval));
	CHECK_EQUAL(8, static_cast<int>(settings.stopAction.interval));
	settings.startAction.mode = ActionMode::Immediate;
	CHECK_EQUAL(static_cast<int>(ActionMode::Synced), static_cast<int>(settings.stopAction.mode));
}

TEST(LoopSettings, trackDefaultsAndSlots) {
	const auto settings = defaultSettings();
	constexpr Colour expected[] = {Colour::Yellow,  Colour::Cyan,  Colour::Blue, Colour::Purple,
	                               Colour::Magenta, Colour::White, Colour::Gold, Colour::Lime};
	for (SlotId slot = 0; slot < 8; ++slot) {
		const auto track = defaultTrackSettings(slot);
		CHECK_EQUAL(static_cast<int>(Source::Left), static_cast<int>(track.source));
		CHECK_EQUAL(static_cast<int>(Monitoring::On), static_cast<int>(track.monitoring));
		CHECK_EQUAL(0, track.recordDurationBars);
		CHECK_EQUAL(static_cast<int>(PlaybackMode::Loop), static_cast<int>(track.playback));
		CHECK_EQUAL(static_cast<int>(expected[slot]), static_cast<int>(track.colour));
		CHECK_EQUAL(static_cast<int>(expected[slot]), static_cast<int>(settings.tracks[slot].colour));
		CHECK_EQUAL(0, track.transposeCents);
	}
	CHECK(validSlot(0));
	CHECK(validSlot(7));
	CHECK_FALSE(validSlot(8));
	CHECK_FALSE(validSlot(255));
}

TEST(LoopSettings, handlesRequireGenerationAndValidSlot) {
	CHECK_FALSE(PhraseHandle{}.valid());
	CHECK_FALSE(SnapshotId{}.valid());
	CHECK_FALSE(RestoreId{}.valid());
	CHECK_FALSE((PhraseHandle{0, 0}.valid()));
	CHECK_FALSE((PhraseHandle{8, 1}.valid()));
	CHECK((PhraseHandle{7, 1}.valid()));
	CHECK(SnapshotId{1}.valid());
	CHECK(RestoreId{1}.valid());
}

TEST(LoopSettings, intervalIdsRoundTripMenuOrder) {
	constexpr Interval expected[] = {Interval::Bar,
	                                 Interval::Half,
	                                 Interval::Quarter,
	                                 Interval::Eighth,
	                                 Interval::Sixteenth,
	                                 Interval::ThirtySecond,
	                                 Interval::BarTriplet,
	                                 Interval::HalfTriplet,
	                                 Interval::QuarterTriplet,
	                                 Interval::EighthTriplet,
	                                 Interval::SixteenthTriplet,
	                                 Interval::ThirtySecondTriplet};
	constexpr int ids[] = {0, 1, 2, 3, 8, 9, 4, 5, 6, 7, 10, 11};
	for (unsigned i = 0; i < 12; ++i) {
		CHECK_EQUAL(ids[i], static_cast<int>(expected[i]));
		CHECK_EQUAL(static_cast<int>(expected[i]), static_cast<int>(intervalAtMenuIndex(i)));
		CHECK_EQUAL(i, menuIndexOfInterval(expected[i]));
	}
}
