#include "CppUTest/TestHarness.h"
#include "loop_station/audio/audio.h"
#include "loop_station/engine/engine.h"
#include "loop_station/settings/adapters.h"
#include "loop_station/view/geometry.h"

#include <array>
#include <limits>

using namespace deluge::loop_station::core;

namespace {
struct Delegate final : EngineDelegate {
	bool preflightResult = true;
	bool beginResult = true;
	CaptureResult finishResult{};
	int begins = 0;
	int finishes = 0;
	int aborts = 0;
	int clears = 0;
	bool preflight(SlotId, bool) override { return preflightResult; }
	bool beginCapture(SlotId, bool, int64_t, int64_t) override {
		++begins;
		return beginResult;
	}
	CaptureResult endCapture(SlotId, bool, int64_t, bool) override {
		++finishes;
		return finishResult;
	}
	void abortProvisionalCapture(SlotId, bool) override { ++aborts; }
	void publishStableState(SlotId, TrackState, int64_t) override {}
	bool navigateRevision(SlotId, bool, bool) override { return true; }
	void clearRevisions(SlotId) override { ++clears; }
};
TransportTime at(int64_t tick, int64_t bar = 96) {
	return {.performedTick = tick, .barLength = bar, .eighthLength = bar / 8, .running = true};
}
} // namespace

TEST_GROUP(LoopStationModel){};

TEST(LoopStationModel, DefaultsAndSanitization) {
	SongModel model;
	CHECK(model.global.recordQuantize == RecordQuantizeMode::SMART);
	CHECK(model.global.startFromStopped == StartFromStoppedMode::RESUME_QUANTIZED);
	model.global.recordQuantize = static_cast<RecordQuantizeMode>(99);
	model.tracks[0].settings.semitones = 100;
	model.tracks[0].settings.cents = -100;
	model.tracks[0].settings.fadeBars = 0;
	model.tracks[7].settings.progressColour = ProgressColour::PINK;
	model.sanitize();
	CHECK(model.global.recordQuantize == RecordQuantizeMode::SMART);
	LONGS_EQUAL(24, model.tracks[0].settings.semitones);
	LONGS_EQUAL(-50, model.tracks[0].settings.cents);
	LONGS_EQUAL(1, model.tracks[0].settings.fadeBars);
	CHECK(model.tracks[7].settings.progressColour == ProgressColour::PINK);
	CHECK(model.tracks[6].settings.progressColour == ProgressColour::GOLD);
}

TEST(LoopStationModel, HistoryPreservesRedoUntilSuccessfulCommit) {
	HistoryState history{.cursor = 0, .size = 3};
	CHECK(history.beginCommit());
	history.rejectCommit();
	LONGS_EQUAL(3, history.size);
	LONGS_EQUAL(0, history.cursor);
	CHECK(history.beginCommit());
	history.acceptCommit();
	LONGS_EQUAL(2, history.size);
	LONGS_EQUAL(1, history.cursor);
}

TEST(LoopStationModel, PhaseMathSupportsNegativeAndRotatedOrigins) {
	LONGS_EQUAL(75, logicalPhaseAt(25, 50, 100));
	Revision revision{.startFrame = 10, .endFrame = 1010, .phraseLengthTicks = 100, .sampleZeroPhaseTicks = 25};
	UNSIGNED_LONGS_EQUAL(510, tickPhaseToFrame(75, revision));
}

TEST_GROUP(LoopStationAudio){};

TEST(LoopStationAudio, ReservesEightStereoRecorders) {
	audio::RecorderBudget budget;
	for (size_t i = 0; i < kSlotCount; ++i)
		CHECK(budget.reserve({static_cast<SlotId>(i), 2}));
	LONGS_EQUAL(16, budget.used());
	budget.release(SlotId::FOUR);
	LONGS_EQUAL(14, budget.used());
}

TEST(LoopStationAudio, DryMixSaturatesPromotesMonoAndPreservesSilence) {
	std::array<int32_t, 2> mono{123, std::numeric_limits<int32_t>::max()};
	std::array<int32_t, 4> stereoInput{0, 0, 1, 1};
	std::array<int32_t, 4> output{};
	auto result = audio::mixDryCumulative(mono, 1, stereoInput, 2, output);
	CHECK(result.accepted);
	LONGS_EQUAL(2, result.channels);
	LONGS_EQUAL(123, output[0]);
	LONGS_EQUAL(123, output[1]);
	LONGS_EQUAL(std::numeric_limits<int32_t>::max(), output[2]);
	LONGS_EQUAL(std::numeric_limits<int32_t>::max(), output[3]);
	result = audio::mixDryCumulative({}, 1, stereoInput, 2, output);
	CHECK_FALSE(result.accepted);
}

TEST(LoopStationAudio, RouterFansOutAndPrioritizesCapturingMonitor) {
	audio::PhysicalInputRouter router;
	router.subscribe({SlotId::ONE, PhysicalSource::STEREO, 1, true, false, true});
	router.subscribe({SlotId::TWO, PhysicalSource::STEREO, 1, false, true, true});
	router.subscribe({SlotId::THREE, PhysicalSource::RIGHT, 2, false, true, true});
	LONGS_EQUAL(2, router.sourceReadCount());
	CHECK(router.monitorAudible(SlotId::ONE));
	CHECK_FALSE(router.monitorAudible(SlotId::TWO));
	CHECK(router.monitorAudible(SlotId::THREE));
	LONGS_EQUAL(2, router.monitorReturnCount());
}

TEST(LoopStationAudio, TransposeUsesQ24UnityAndGainRampUsesTicks) {
	LONGS_EQUAL(kTransposeUnity, audio::transposeRatioQ24(0, 0));
	CHECK(audio::transposeRatioQ24(12, 0) > kTransposeUnity);
	audio::GainRamp ramp{0, 100, 0, kTransposeUnity};
	LONGS_EQUAL(kTransposeUnity / 2, ramp.gainAt(50));
}

TEST_GROUP(LoopStationEngine){};

TEST_GROUP(LoopStationView){};

TEST(LoopStationView, GeometryMapsGridAndCoalescesFourCentrePads) {
	using namespace deluge::loop_station::core::view;
	CHECK(Geometry::origin(SlotId::ONE) == (Pad{0, 4}));
	CHECK(Geometry::origin(SlotId::EIGHT) == (Pad{12, 0}));
	CHECK(Geometry::slotAt(0, 7) == SlotId::ONE);
	CHECK(Geometry::slotAt(15, 7) == SlotId::FOUR);
	CHECK(Geometry::slotAt(0, 0) == SlotId::FIVE);
	CHECK(Geometry::slotAt(15, 0) == SlotId::EIGHT);
	LONGS_EQUAL(6, Geometry::progressPad(50, 100));
	CentreCoalescer coalescer;
	auto firstDown = coalescer.update(1, 5, true);
	CHECK(firstDown.down);
	CHECK(firstDown.slot == SlotId::ONE);
	CHECK_FALSE(coalescer.update(2, 5, true).down);
	CHECK_FALSE(coalescer.update(1, 5, false).up);
	CHECK(coalescer.update(2, 5, false).up);
}

TEST(LoopStationEngine, QuantizationBoundariesAreDeterministic) {
	LONGS_EQUAL(96, Engine::nextBar(96, 96, true));
	LONGS_EQUAL(192, Engine::nextBar(96, 96, false));
	LONGS_EQUAL(96, Engine::smartStop(101, 96, 12));
	LONGS_EQUAL(130, Engine::smartStop(130, 96, 12));
}

TEST(LoopStationEngine, InitialTakeOverdubAndBackgroundFinish) {
	SongModel model;
	Delegate delegate;
	Engine engine(model, delegate);
	engine.centreDown(SlotId::ONE, at(10), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::RECORDING);
	engine.centreDown(SlotId::ONE, at(110), 1500);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	LONGS_EQUAL(100, engine.track(SlotId::ONE).phraseLengthTicks);
	engine.centreDown(SlotId::ONE, at(120), 2000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	engine.centreDown(SlotId::ONE, at(130), 2500);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK(engine.track(SlotId::ONE).capturePhase == CapturePhase::FINISHING_OVERDUB);
	engine.process(at(220));
	LONGS_EQUAL(2, delegate.finishes);
}

TEST(LoopStationEngine, DoublePressAbortsProvisionalOverdubAndStops) {
	SongModel model;
	model.tracks[0].settings.stopMode = StopMode::IMMEDIATE;
	Delegate delegate;
	Engine engine(model, delegate);
	engine.restore(SlotId::ONE, 96, 0, true);
	engine.centreDown(SlotId::ONE, at(20), 1000);
	engine.centreDown(SlotId::ONE, at(25), 1200);
	LONGS_EQUAL(1, delegate.aborts);
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
}

TEST(LoopStationEngine, StoppedClearHoldRetainsBindingAndSettings) {
	SongModel model;
	model.tracks[0].clipBinding = 1;
	model.tracks[0].settings.fadeBars = 4;
	Delegate delegate;
	Engine engine(model, delegate);
	engine.restore(SlotId::ONE, 96, 0, false);
	engine.clearHold(SlotId::ONE);
	CHECK(engine.track(SlotId::ONE).state == TrackState::EMPTY);
	LONGS_EQUAL(1, model.tracks[0].clipBinding);
	LONGS_EQUAL(4, model.tracks[0].settings.fadeBars);
	LONGS_EQUAL(1, delegate.clears);
}
