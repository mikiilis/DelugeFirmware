#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/timing.h"
using namespace loop_station;
TEST_GROUP(LoopTiming){};
TEST(LoopTiming, everyIntervalUsesFuturePulseCountAndCeiling) {
	ClockMap clock;
	const SampleClock expected[] = {88200, 44100, 22050, 11025, 58800, 29400, 14700, 7350, 5513, 2757, 3675, 1838};
	for (unsigned id = 0; id < 12; ++id)
		CHECK_EQUAL(expected[id],
		            nextDeadline({ActionMode::Synced, static_cast<Interval>(id)}, 0, clock).sampleAt(clock));
	// Bar means four future quarter pulses, rather than next bar boundary.
	CHECK_EQUAL(88200, nextDeadline({ActionMode::Synced, Interval::Bar}, 1000, clock).sampleAt(clock));
	CHECK_EQUAL(110250, nextDeadline({ActionMode::Synced, Interval::Bar}, 22050, clock).sampleAt(clock));
	CHECK_EQUAL(44100, nextDeadline({ActionMode::Synced, Interval::Quarter}, 22050, clock).sampleAt(clock));
	CHECK_EQUAL(17, nextDeadline({ActionMode::Immediate, Interval::Bar}, 17, clock).sampleAt(clock));
}
TEST(LoopTiming, offsetAndInaudibleMetronomeUseSameMap) {
	ClockMap clock{100};
	CHECK_EQUAL(22150, nextDeadline({ActionMode::Synced, Interval::Quarter}, 100, clock).sampleAt(clock));
	CHECK_EQUAL(100, nextDeadline({ActionMode::Synced, Interval::Quarter}, 0, clock).sampleAt(clock));
}
TEST(LoopTiming, tempoEditsPreserveExactPhaseAndRetimePendingActions) {
	ClockMap clock;
	const auto action = nextDeadline({ActionMode::Synced, Interval::Quarter}, 0, clock);
	clock.setTempo(11025, 60);
	CHECK_EQUAL(33075, action.sampleAt(clock));
	CHECK_EQUAL(176400, clock.samplesForBars(1));
	CHECK_EQUAL(22050, action.sampleAt(ClockMap{}));
}
TEST(LoopTiming, tripletRoundingDoesNotAccumulateAndExactPulsesWait) {
	ClockMap clock;
	CHECK_EQUAL(22050, nextDeadline({ActionMode::Synced, Interval::ThirtySecondTriplet}, 20213, clock).sampleAt(clock));
	CHECK_EQUAL(23888, nextDeadline({ActionMode::Synced, Interval::ThirtySecondTriplet}, 22050, clock).sampleAt(clock));
	CHECK_EQUAL(88200, nextDeadline({ActionMode::Synced, Interval::SixteenthTriplet}, 84525, clock).sampleAt(clock));
	// 127 BPM is deliberately not representable in Q16 sample positions.
	clock.setTempo(0, 127);
	SampleClock now = 0;
	for (unsigned i = 0; i < 1200; ++i)
		now = nextDeadline({ActionMode::Synced, Interval::ThirtySecondTriplet}, now, clock).sampleAt(clock);
	CHECK_EQUAL(2083465, now); // ceil(1200 * 2646000 / (127*12))
}
TEST(LoopTiming, localStoppedClockAdvancesAndTransportStartAnchors) {
	ClockMap clock;
	clock.setTransport(false, 1000);
	CHECK_EQUAL(22050, nextDeadline({ActionMode::Synced, Interval::Quarter}, 20000, clock).sampleAt(clock));
	clock.setTransport(true, 30000);
	CHECK_EQUAL(52050, nextDeadline({ActionMode::Synced, Interval::Quarter}, 30000, clock).sampleAt(clock));
	clock.setTransport(false, 31000);
	CHECK_EQUAL(52050, nextDeadline({ActionMode::Synced, Interval::Quarter}, 40000, clock).sampleAt(clock));
}
TEST(LoopTiming, timerWrapExtendsMonotonically) {
	SampleClockExtender timer;
	CHECK_EQUAL(4294967290ULL, timer.observe(0xfffffffa));
	CHECK_EQUAL(4294967300ULL, timer.observe(4));
	CHECK_EQUAL(4294967310ULL, timer.observe(14));
}

TEST(LoopTiming, q16IntegrationRetainsSubTickRemainderAcrossTempoEdits) {
	ClockMap clock;
	CHECK_EQUAL(2972, clock.quarterQ16(1000));
	clock.setTempoQ16(1000, 120 * 65536 + 32768);
	CHECK_EQUAL(21963, nextDeadline({ActionMode::Synced, Interval::Quarter}, 1000, clock).sampleAt(clock));
}

TEST(LoopTiming, transportAnchorPreservesExistingLocalDeadlinesAndDuration) {
	ClockMap clock;
	const auto pending = nextDeadline({ActionMode::Synced, Interval::Quarter}, 0, clock);
	const auto duration = durationDeadline(1, 0, clock);
	clock.setTransport(true, 10000);
	CHECK_EQUAL(22050, pending.sampleAt(clock));
	CHECK_EQUAL(88200, duration.sampleAt(clock));
	CHECK_EQUAL(32050, nextDeadline({ActionMode::Synced, Interval::Quarter}, 10000, clock).sampleAt(clock));
	clock.setTempo(11025, 60);
	CHECK_EQUAL(33075, pending.sampleAt(clock));
	CHECK_EQUAL(53075, nextDeadline({ActionMode::Synced, Interval::Quarter}, 11025, clock).sampleAt(clock));
}

TEST(LoopTiming, metronomeOffsetBeyondOnePulseRetainsEarlierFuturePulses) {
	ClockMap clock{100000};
	CHECK_EQUAL(11800, nextDeadline({ActionMode::Synced, Interval::Quarter}, 0, clock).sampleAt(clock));
	CHECK_EQUAL(775, nextDeadline({ActionMode::Synced, Interval::Sixteenth}, 0, clock).sampleAt(clock));
}
