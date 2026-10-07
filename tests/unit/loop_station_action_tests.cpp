#include "CppUTest/TestHarness.h"
#include "loop_station/loop_station.h"
#include "loop_station/scheduler.h"
using namespace deluge::loop_station;

namespace {
Transport timeAt(uint32_t tick, uint32_t bar = 96) {
	return {.tick = tick, .barLength = bar, .playing = true};
}
struct ActionDelegate : EngineDelegate {
	int begins = 0, ends = 0, gates = 0;
	bool ready = false, async = false, ended = false, rejectDub = false;
	uint32_t lastBegin = 0, lastLength = 0;
	Error beginCapture(SlotId, bool overdub, uint32_t tick) override {
		++begins;
		lastBegin = tick;
		return overdub && rejectDub ? Error::ALLOCATION : Error::NONE;
	}
	CaptureFinish finishCapture(SlotId, bool, uint32_t length, bool) override {
		++ends;
		lastLength = length;
		return {.asynchronous = async};
	}
	void endOverdubInput(SlotId) override { ++gates; }
	bool overdubReady(SlotId) override { return ready; }
	bool playbackEnded(SlotId) override { return ended; }
};
} // namespace
TEST_GROUP(LoopStationActions){};

TEST(LoopStationActions, DefaultsAndAllIntervalRoundTrips) {
	SongModel model;
	CHECK(model.global.startAction.mode == ActionMode::SYNCED);
	CHECK(model.global.startAction.division == ActionDivision::SIXTEENTH);
	CHECK_FALSE(model.global.startAction.triplet);
	CHECK(model.global.stopAction.division == ActionDivision::SIXTEENTH);
	CHECK_FALSE(model.global.stopAction.triplet);
	CHECK(model.global.stopAction.mode == ActionMode::SYNCED);
	for (auto field : {GlobalParameter::START_INTERVAL, GlobalParameter::STOP_INTERVAL}) {
		for (int value = 0; value < 12; ++value) {
			writeGlobalParameter(model, field, value);
			CHECK_EQUAL(value, readGlobalParameter(model, field));
		}
		for (int invalid : {-1, 12, 256, 65538}) {
			writeGlobalParameter(model, field, invalid);
			CHECK_EQUAL(4, readGlobalParameter(model, field));
		}
	}
	writeGlobalParameter(model, GlobalParameter::START_ACTION, 256);
	writeGlobalParameter(model, GlobalParameter::STOP_ACTION, -1);
	CHECK(model.global.startAction.mode == ActionMode::SYNCED);
	CHECK(model.global.stopAction.mode == ActionMode::SYNCED);
}

TEST(LoopStationActions, CountsFutureClicksInsteadOfNextBar) {
	CHECK_EQUAL(144, Engine::actionTick(timeAt(50), {ActionMode::SYNCED, ActionDivision::BAR}));
	CHECK_EQUAL(96, Engine::actionTick(timeAt(50), {ActionMode::SYNCED, ActionDivision::HALF}));
	CHECK_EQUAL(72, Engine::actionTick(timeAt(50), {ActionMode::SYNCED, ActionDivision::QUARTER}));
	CHECK_EQUAL(60, Engine::actionTick(timeAt(50), {ActionMode::SYNCED, ActionDivision::EIGHTH}));
	CHECK_EQUAL(48, Engine::actionTick(timeAt(24), {ActionMode::SYNCED, ActionDivision::QUARTER}));
	CHECK_EQUAL(24, Engine::actionTick(timeAt(0), {ActionMode::SYNCED, ActionDivision::QUARTER}));
	auto offset = timeAt(20);
	offset.metronomeOffset = 12;
	CHECK_EQUAL(36, Engine::actionTick(offset, {ActionMode::SYNCED, ActionDivision::QUARTER}));
}

TEST(LoopStationActions, SavedIntervalIdsPreserveLegacyTripletsAndAppendShortDivisions) {
	constexpr ActionDivision divisions[] = {
	    ActionDivision::BAR,           ActionDivision::HALF,      ActionDivision::QUARTER,
	    ActionDivision::EIGHTH,        ActionDivision::BAR,       ActionDivision::HALF,
	    ActionDivision::QUARTER,       ActionDivision::EIGHTH,    ActionDivision::SIXTEENTH,
	    ActionDivision::THIRTY_SECOND, ActionDivision::SIXTEENTH, ActionDivision::THIRTY_SECOND};
	for (int id = 0; id < 12; ++id) {
		ActionTiming timing{ActionMode::IMMEDIATE};
		decodeActionInterval(timing, id);
		CHECK(timing.mode == ActionMode::IMMEDIATE);
		CHECK(timing.division == divisions[id]);
		CHECK_EQUAL((id >= 4 && id < 8) || id >= 10, timing.triplet);
		CHECK_EQUAL(id, encodeActionInterval(timing));
		SongModel model;
		model.global.startAction = timing;
		model.sanitize();
		CHECK_EQUAL(id, encodeActionInterval(model.global.startAction));
		writeGlobalParameter(model, GlobalParameter::STOP_INTERVAL,
		                     readGlobalParameter(model, GlobalParameter::START_INTERVAL));
		CHECK_EQUAL(id, encodeActionInterval(model.global.stopAction));
	}
	for (int invalid : {-1, 12, 256, 65538}) {
		ActionTiming timing{ActionMode::IMMEDIATE, ActionDivision::BAR, true};
		decodeActionInterval(timing, invalid);
		CHECK(timing.mode == ActionMode::IMMEDIATE);
		CHECK(timing.division == ActionDivision::SIXTEENTH);
		CHECK_FALSE(timing.triplet);
	}
	SongModel model;
	for (auto* timing : {&model.global.startAction, &model.global.stopAction}) {
		*timing = {static_cast<ActionMode>(99), static_cast<ActionDivision>(99), true};
	}
	model.sanitize();
	for (auto timing : {model.global.startAction, model.global.stopAction}) {
		CHECK(timing.mode == ActionMode::SYNCED);
		CHECK(timing.division == ActionDivision::SIXTEENTH);
		CHECK_FALSE(timing.triplet);
	}
}

TEST(LoopStationActions, ShortIntervalsRetainFractionalDeadlinesAndMetronomeOffset) {
	for (uint32_t bar : {4u, 20u, 96u, 192u, 384u}) {
		for (auto division : {ActionDivision::SIXTEENTH, ActionDivision::THIRTY_SECOND}) {
			for (bool triplet : {false, true}) {
				ActionTiming timing{ActionMode::SYNCED, division, triplet};
				uint64_t numerator = static_cast<uint64_t>(bar) * 65536 * (triplet ? 2 : 1);
				uint64_t denominator = (1u << static_cast<uint32_t>(division)) * (triplet ? 3 : 1);
				for (uint32_t offset : {0u, 1u, 7u}) {
					// Start at a later pulse, including pulses between whole song ticks.
					uint64_t pulseNumber = 256;
					uint64_t position = (pulseNumber * numerator + denominator - 1) / denominator
					                    - (static_cast<uint64_t>(offset) << 16);
					uint64_t expected = ((pulseNumber + 1) * numerator + denominator - 1) / denominator
					                    - (static_cast<uint64_t>(offset) << 16);
					auto now = timeAt(position >> 16, bar);
					now.fractionQ16 = position & 65535;
					now.metronomeOffset = offset;
					CHECK_EQUAL(expected, Engine::actionPositionQ16(now, timing));
					now.tick = (expected - 1) >> 16;
					now.fractionQ16 = (expected - 1) & 65535;
					CHECK_EQUAL(expected, Engine::actionPositionQ16(now, timing));
				}
			}
		}
	}
}

TEST(LoopStationActions, ShortIntervalsGateCaptureAndGroupActionsAtExactDeadlines) {
	for (auto division : {ActionDivision::SIXTEENTH, ActionDivision::THIRTY_SECOND}) {
		for (bool triplet : {false, true}) {
			SongModel model;
			model.global.startAction = model.global.stopAction = {ActionMode::SYNCED, division, triplet};
			ActionDelegate delegate;
			Engine engine(model, &delegate);
			auto now = timeAt(0, 20);
			auto advance = [&](uint64_t position) {
				now.tick = position >> 16;
				now.fractionQ16 = position & 65535;
				engine.processTick(now);
			};
			engine.centrePressed(SlotId::ONE, now, 0);
			auto start = engine.nextActionPositionQ16();
			advance(start - 1);
			CHECK_EQUAL(0, delegate.begins);
			advance(start);
			CHECK_EQUAL(1, delegate.begins);
			engine.centrePressed(SlotId::ONE, now, 1000);
			auto end = engine.nextActionPositionQ16();
			CHECK(end > start);
			advance(end - 1);
			CHECK_EQUAL(0, delegate.ends);
			advance(end);
			CHECK_EQUAL(1, delegate.ends);
			CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
			for (auto slot : {SlotId::TWO, SlotId::THREE})
				engine.restoreBoundPhrase(slot, slot == SlotId::TWO ? 48 : 96, false);
			engine.startAll(now);
			auto play = engine.nextActionPositionQ16();
			advance(play - 1);
			for (auto slot : {SlotId::TWO, SlotId::THREE})
				CHECK(engine.track(slot).state == TrackState::STOPPED);
			advance(play);
			engine.stopAll(now);
			auto stop = engine.nextActionPositionQ16();
			advance(stop - 1);
			for (auto slot : {SlotId::ONE, SlotId::TWO, SlotId::THREE})
				CHECK(engine.track(slot).state == TrackState::PLAYING);
			advance(stop);
			for (auto slot : {SlotId::ONE, SlotId::TWO, SlotId::THREE})
				CHECK(engine.track(slot).state == TrackState::STOPPED);
		}
	}
}

TEST(LoopStationActions, TripletsAndModesAcrossClockResolutions) {
	for (uint32_t bar : {96u, 192u, 384u}) {
		for (bool triplet : {false, true}) {
			for (uint32_t division = 0; division < 6; ++division) {
				ActionTiming timing{ActionMode::SYNCED, static_cast<ActionDivision>(division), triplet};
				uint32_t interval = bar / (1u << division);
				if (triplet)
					interval = interval * 2 / 3;
				CHECK_EQUAL(interval, Engine::actionTick(timeAt(0, bar), timing));
				timing.mode = ActionMode::IMMEDIATE;
				CHECK_EQUAL(17, Engine::actionTick(timeAt(17, bar), timing));
			}
		}
	}
	CHECK_EQUAL(17, Engine::actionTick(timeAt(17, 0), {}));
}

TEST(LoopStationActions, RequestedRecordPlayAndDoubleStopSequence) {
	SongModel model;
	model.global.startAction.division = ActionDivision::QUARTER;
	model.global.stopAction = {ActionMode::SYNCED, ActionDivision::BAR};
	ActionDelegate delegate;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, timeAt(10), 0);
	CHECK(engine.track(SlotId::ONE).event.action == PendingAction::START_RECORDING);
	engine.processTick(timeAt(23));
	CHECK_EQUAL(0, delegate.begins);
	engine.processTick(timeAt(24));
	CHECK_EQUAL(1, delegate.begins);
	engine.centrePressed(SlotId::ONE, timeAt(50), 1000);
	CHECK_EQUAL(144, engine.track(SlotId::ONE).event.tick);
	engine.processTick(timeAt(143));
	CHECK_EQUAL(0, delegate.ends);
	engine.processTick(timeAt(144));
	CHECK_EQUAL(1, delegate.ends);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	engine.centrePressed(SlotId::ONE, timeAt(150), 2000);
	engine.centrePressed(SlotId::ONE, timeAt(151), 2100);
	CHECK_EQUAL(240, engine.track(SlotId::ONE).event.tick);
	engine.processTick(timeAt(239));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	engine.processTick(timeAt(240));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
}

TEST(LoopStationActions, CancelDoesNotRemoveAutomaticRecordLimit) {
	SongModel model;
	model.tracks[0].settings.durationBars = 1;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.stopAction = {ActionMode::SYNCED, ActionDivision::BAR};
	ActionDelegate delegate;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, timeAt(0), 0);
	engine.centrePressed(SlotId::ONE, timeAt(30), 1000);
	engine.centrePressed(SlotId::ONE, timeAt(31), 1200);
	CHECK_FALSE(engine.track(SlotId::ONE).event.pending());
	CHECK_EQUAL(96, engine.track(SlotId::ONE).automaticEndTick);
	engine.processTick(timeAt(96));
	CHECK_EQUAL(1, delegate.ends);
}

TEST(LoopStationActions, ContinuousOverdubHasNoMusicalEndAndStopsInputImmediately) {
	SongModel model;
	ActionDelegate delegate;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true);
	engine.centrePressed(SlotId::ONE, timeAt(12), 0);
	engine.processTick(timeAt(10000));
	CHECK_EQUAL(0, delegate.ends);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	engine.centrePressed(SlotId::ONE, timeAt(10001), 5000);
	CHECK_EQUAL(1, delegate.gates);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	engine.serviceAudioCompletion(timeAt(10001));
	CHECK_EQUAL(0, delegate.ends);
	delegate.ready = true;
	engine.serviceAudioCompletion(timeAt(10002));
	CHECK_EQUAL(1, delegate.ends);
	CHECK_EQUAL(2, engine.track(SlotId::ONE).historySize);
}

TEST(LoopStationActions, OverdubStartsImmediatelyWhilePreviousGenerationIsSaving) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	ActionDelegate delegate;
	delegate.async = true;
	delegate.ready = true; // Sealing is immediate; async means the WAV is not saved.
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true);
	engine.centrePressed(SlotId::ONE, timeAt(0), 0);
	CHECK_EQUAL(1, delegate.begins);
	engine.centrePressed(SlotId::ONE, timeAt(1), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	engine.centrePressed(SlotId::ONE, timeAt(2), 2000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	CHECK_EQUAL(2, delegate.begins);
	CHECK_EQUAL(2, engine.track(SlotId::ONE).captureStartTick);
	CHECK(engine.track(SlotId::ONE).committingLength != 0);
	engine.centrePressed(SlotId::ONE, timeAt(3), 3000);
	CHECK_EQUAL(2, engine.track(SlotId::ONE).pendingCommits);
	engine.centrePressed(SlotId::ONE, timeAt(4), 4000);
	CHECK_EQUAL(3, delegate.begins);
	engine.notifyCommitComplete(SlotId::ONE, true);
	CHECK_EQUAL(1, engine.track(SlotId::ONE).pendingCommits);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	CHECK_EQUAL(4, engine.track(SlotId::ONE).captureStartTick);
	engine.notifyCommitComplete(SlotId::ONE, true);
	CHECK_EQUAL(0, engine.track(SlotId::ONE).committingLength);
}

TEST(LoopStationActions, StopDuringSavingClosesNewInputWithoutRestartingCapture) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.stopAction.mode = ActionMode::IMMEDIATE;
	ActionDelegate delegate;
	delegate.async = true;
	delegate.ready = true;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true);
	engine.centrePressed(SlotId::ONE, timeAt(0), 0);
	engine.centrePressed(SlotId::ONE, timeAt(1), 1000);
	engine.centrePressed(SlotId::ONE, timeAt(2), 2000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	engine.stopAll(timeAt(3));
	engine.serviceAudioCompletion(timeAt(4));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	CHECK_EQUAL(2, delegate.begins);
	CHECK_EQUAL(2, engine.track(SlotId::ONE).pendingCommits);
}

TEST(LoopStationActions, CommitDoesNotDelayDependentOverdubOrResetItsPosition) {
	SongModel model;
	model.global.stopAction.mode = ActionMode::IMMEDIATE;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_OVERDUB_PLAY;
	ActionDelegate delegate;
	delegate.async = true;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, timeAt(0), 0);
	engine.centrePressed(SlotId::ONE, timeAt(48), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	CHECK_EQUAL(2, delegate.begins);
	CHECK_EQUAL(48, delegate.lastBegin);
	CHECK_EQUAL(0, engine.track(SlotId::ONE).historySize);
	engine.notifyCommitComplete(SlotId::ONE, true, Error::NONE);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	CHECK_EQUAL(48, engine.track(SlotId::ONE).overdubStartTick);
	CHECK_EQUAL(1, engine.track(SlotId::ONE).historySize);
}

TEST(LoopStationActions, StopAllIncludesRecordingOverdubAndPendingStarts) {
	SongModel model;
	model.global.stopAction.mode = ActionMode::IMMEDIATE;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	ActionDelegate delegate;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, timeAt(0), 0);
	engine.restoreBoundPhrase(SlotId::TWO, 96, true);
	engine.centrePressed(SlotId::TWO, timeAt(10), 0);
	model.global.startAction.mode = ActionMode::SYNCED;
	engine.centrePressed(SlotId::THREE, timeAt(10), 0);
	engine.stopAll(timeAt(20));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	CHECK(engine.track(SlotId::TWO).state == TrackState::STOPPED);
	CHECK_EQUAL(1, delegate.gates);
	CHECK_FALSE(engine.track(SlotId::THREE).event.pending());
}

TEST(LoopStationActions, OneShotEndComesFromAudioInsteadOfTempoTicks) {
	SongModel model;
	model.tracks[0].settings.playback = PlaybackMode::ONE_SHOT;
	ActionDelegate delegate;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true);
	engine.processTick(timeAt(960));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	delegate.ended = true;
	engine.serviceAudioCompletion(timeAt(961));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
}

TEST(LoopStationActions, FractionalEighthAndTripletDeadlinesDoNotRoundToSongTicks) {
	SongModel model;
	model.global.startAction = {ActionMode::SYNCED, ActionDivision::EIGHTH};
	ActionDelegate delegate;
	Engine engine(model, &delegate);
	// Five ticks per quarter: a half beat is exactly 2.5 ticks.
	engine.centrePressed(SlotId::ONE, timeAt(0, 20), 0);
	CHECK_EQUAL(163840, engine.nextActionPositionQ16());
	auto now = timeAt(2, 20);
	now.fractionQ16 = 32767;
	engine.processTick(now);
	CHECK_EQUAL(0, delegate.begins);
	now.fractionQ16 = 32768;
	engine.processTick(now);
	CHECK_EQUAL(1, delegate.begins);
	CHECK_FALSE(engine.track(SlotId::ONE).event.pending());
	auto exact = timeAt(2, 20);
	exact.fractionQ16 = 32768;
	CHECK_EQUAL(5ull << 16, Engine::actionPositionQ16(exact, model.global.startAction));
	CHECK_EQUAL((5ull * 65536 + 2) / 3,
	            Engine::actionPositionQ16(timeAt(0, 20), {ActionMode::SYNCED, ActionDivision::EIGHTH, true}));
}

TEST(LoopStationActions, TransportShutdownCancelsWaitingStartsAndDrainsCaptures) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.stopAction = {ActionMode::SYNCED, ActionDivision::BAR};
	model.global.recordAction = RecordAction::RECORD_OVERDUB_PLAY;
	ActionDelegate delegate;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, timeAt(0), 0);
	engine.restoreBoundPhrase(SlotId::TWO, 96, true);
	engine.centrePressed(SlotId::TWO, timeAt(10), 0);
	model.global.startAction.mode = ActionMode::SYNCED;
	engine.centrePressed(SlotId::THREE, timeAt(10), 0);
	engine.transportStopped(timeAt(20));
	CHECK_EQUAL(1, delegate.ends);
	CHECK_EQUAL(1, delegate.gates);
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	CHECK(engine.track(SlotId::TWO).state == TrackState::STOPPED);
	CHECK_FALSE(engine.track(SlotId::THREE).event.pending());
	delegate.ready = true;
	engine.serviceAudioCompletion(timeAt(20));
	CHECK_EQUAL(2, delegate.ends);
	CHECK(engine.track(SlotId::TWO).state == TrackState::STOPPED);
}

TEST(LoopStationActions, CompletingOverdubPreservesPendingPlaybackStop) {
	SongModel model;
	model.global.stopAction = {ActionMode::SYNCED, ActionDivision::BAR};
	ActionDelegate delegate;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true);
	engine.centrePressed(SlotId::ONE, timeAt(10), 0);
	engine.stopAll(timeAt(20));
	delegate.ready = true;
	engine.serviceAudioCompletion(timeAt(21));
	CHECK(engine.track(SlotId::ONE).event.action == PendingAction::STOP);
	engine.processTick(timeAt(96));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
}

TEST(LoopStationActions, PendingCommitStillUsesConfiguredStopAction) {
	SongModel model;
	model.global.stopAction = {ActionMode::SYNCED, ActionDivision::BAR};
	Engine engine(model);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true);
	engine.track(SlotId::ONE).committingLength = 96;
	engine.stopAll(timeAt(20));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK(engine.track(SlotId::ONE).event.action == PendingAction::STOP);
	CHECK_EQUAL(96, engine.track(SlotId::ONE).event.tick);
	engine.processTick(timeAt(96));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
}

TEST(LoopStationActions, AutomaticOverdubRejectionPreservesFirstTakePublication) {
	SongModel model;
	model.global.stopAction.mode = ActionMode::IMMEDIATE;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_OVERDUB_PLAY;
	ActionDelegate delegate;
	delegate.async = true;
	delegate.rejectDub = true;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, timeAt(0), 0);
	engine.centrePressed(SlotId::ONE, timeAt(48), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::ERROR);
	CHECK(engine.track(SlotId::ONE).error == Error::ALLOCATION);
	CHECK_EQUAL(48, engine.track(SlotId::ONE).committingLength);
	engine.notifyCommitComplete(SlotId::ONE, true);
	CHECK_EQUAL(1, engine.track(SlotId::ONE).historySize);
	engine.centrePressed(SlotId::ONE, timeAt(60), 2000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
}

TEST(LoopStationActions, FixedDurationRetainsFractionalRecordingStart) {
	SongModel model;
	model.global.startAction = {ActionMode::SYNCED, ActionDivision::EIGHTH};
	model.tracks[0].settings.durationBars = 1;
	ActionDelegate delegate;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, timeAt(0, 20), 0);
	auto now = timeAt(2, 20);
	now.fractionQ16 = 32768;
	engine.processTick(now);
	now.tick = 22;
	now.fractionQ16 = 32767;
	engine.processTick(now);
	CHECK_EQUAL(0, delegate.ends);
	now.fractionQ16 = 32768;
	engine.processTick(now);
	CHECK_EQUAL(1, delegate.ends);
	CHECK_EQUAL(20, delegate.lastLength);
}

TEST(LoopStationActions, TransportStopPreservesFractionalEndIncludingDeferredStop) {
	for (bool deferred : {false, true}) {
		SongModel model;
		model.global.startAction.mode = ActionMode::IMMEDIATE;
		ActionDelegate delegate;
		Engine engine(model, &delegate);
		auto now = timeAt(2);
		now.fractionQ16 = 49152;
		engine.centrePressed(SlotId::ONE, now, 1000);
		now.tick = 22;
		engine.track(SlotId::ONE).finishingCapture = deferred;
		engine.transportStopped(now);
		if (deferred) {
			CHECK_EQUAL(0, delegate.ends);
			engine.track(SlotId::ONE).finishingCapture = false;
			// Service can happen after the transport position has reset to zero.
			engine.serviceAudioCompletion(Transport{});
		}
		CHECK_EQUAL(1, delegate.ends);
		CHECK_EQUAL(20, delegate.lastLength);
		CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	}
}
