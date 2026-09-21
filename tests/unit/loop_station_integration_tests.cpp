#include "CppUTest/TestHarness.h"
#include "loop_station/loop_station.h"
#include "loop_station/scheduler.h"
#include <functional>

using namespace deluge::loop_station;

namespace {
Transport at(uint32_t tick, uint32_t bar = 96) {
	return {.tick = tick, .barLength = bar, .eighthNoteLength = bar / 8, .playing = true};
}

void finishDub(Engine& engine, uint32_t tick, SlotId slot = SlotId::ONE) {
	if (engine.track(slot).state == TrackState::OVERDUBBING)
		engine.centrePressed(slot, at(tick), 100000 + tick);
	engine.serviceAudioCompletion(at(tick));
}

struct Delegate final : EngineDelegate {
	std::array<int, kTrackCount> preflights{};
	std::array<int, kTrackCount> begins{};
	std::array<int, kTrackCount> finishes{};
	std::array<int, kTrackCount> aborts{};
	std::array<int, kTrackCount> clears{};
	std::array<int, kTrackCount> gainChanges{};
	std::array<int32_t, kTrackCount> lastGains{};
	std::array<uint32_t, kTrackCount> beginTicks{};
	std::array<uint32_t, kTrackCount> finishLengths{};
	std::array<bool, kTrackCount> beginOverdubs{};
	std::array<bool, kTrackCount> finishOverdubs{};
	std::array<bool, kTrackCount> finishCrops{};
	std::array<int, kTrackCount> swaps{};
	std::array<bool, kTrackCount> swapTargetsHavePhrase{};
	bool reject = false;
	bool rejectBegin = false;
	bool rejectFinish = false;
	bool rejectSwap = false;
	bool asynchronous = false;
	uint32_t committedLength = 0;
	Error rejectError = Error::NONE;
	std::function<void()> duringPreflight;
	std::function<void()> duringBegin;
	std::function<void()> duringFinish;
	std::function<void()> duringAbort;

	bool preflight(SlotId slot, bool) override {
		preflights[static_cast<size_t>(slot)]++;
		if (duringPreflight)
			duringPreflight();
		return !reject;
	}
	bool beginCapture(SlotId slot, bool overdub, uint32_t tick) override {
		auto index = static_cast<size_t>(slot);
		begins[index]++;
		beginOverdubs[index] = overdub;
		beginTicks[index] = tick;
		if (duringBegin)
			duringBegin();
		return !rejectBegin;
	}
	CaptureFinish finishCapture(SlotId slot, bool overdub, uint32_t length, bool crop) override {
		auto index = static_cast<size_t>(slot);
		finishes[index]++;
		finishOverdubs[index] = overdub;
		finishLengths[index] = length;
		finishCrops[index] = crop;
		if (duringFinish) {
			duringFinish();
		}
		return {.accepted = !rejectFinish,
		        .asynchronous = asynchronous,
		        .committedLength = committedLength,
		        .error = rejectError};
	}
	void abortCapture(SlotId slot, bool) override {
		aborts[static_cast<size_t>(slot)]++;
		if (duringAbort)
			duringAbort();
	}
	void phraseCleared(SlotId slot) override { clears[static_cast<size_t>(slot)]++; }
	void playbackGainChanged(SlotId slot, int32_t gain) override {
		auto index = static_cast<size_t>(slot);
		gainChanges[index]++;
		lastGains[index] = gain;
	}
	bool navigateHistory(SlotId slot, bool, bool& targetHasPhrase) override {
		auto index = static_cast<size_t>(slot);
		swaps[index]++;
		swapTargetsHavePhrase[index] = targetHasPhrase;
		return !rejectSwap;
	}
};
} // namespace

TEST_GROUP(OperationalLoopStationModel){};

TEST(OperationalLoopStationModel, DefaultsAreSafeAndTrackColoursAreDeterministic) {
	SongModel model;
	CHECK(model.global.recordAction == RecordAction::RECORD_PLAY_OVERDUB);
	CHECK(model.global.startAction.mode == ActionMode::SYNCED);
	CHECK(model.global.dimBorderColour == PadColour::BLUE);

	for (size_t i = 0; i < kTrackCount; ++i) {
		auto const& track = model.tracks[i];
		CHECK_EQUAL(kInvalidSlotBinding, track.clipSlotId);
		CHECK(track.settings.source == SourceType::LEFT);
		CHECK_TRUE(track.settings.monitoring);
		CHECK_EQUAL(kManualDuration, track.settings.durationBars);
		CHECK(track.settings.progressColour == defaultProgressColour(static_cast<SlotId>(i)));
		CHECK_FALSE(isReservedStateColour(track.settings.progressColour));
		CHECK(track.settings.playback == PlaybackMode::LOOP);
	}
	CHECK(model.tracks[6].settings.progressColour == PadColour::GOLD);
	CHECK(model.tracks[7].settings.progressColour == PadColour::LIME);
}

TEST(OperationalLoopStationModel, SanitizesUnknownEnumsAndBoundedValues) {
	SongModel model;
	model.global.recordAction = static_cast<RecordAction>(99);
	model.global.startAction.mode = static_cast<ActionMode>(99);
	model.global.dimBorderColour = static_cast<PadColour>(99);
	auto& settings = model.tracks[3].settings;
	settings.source = static_cast<SourceType>(99);
	settings.progressColour = static_cast<PadColour>(99);
	settings.playback = static_cast<PlaybackMode>(99);

	model.sanitize();

	CHECK(model.global.recordAction == RecordAction::RECORD_PLAY_OVERDUB);
	CHECK(model.global.startAction.mode == ActionMode::SYNCED);
	CHECK(model.global.dimBorderColour == PadColour::BLUE);
	CHECK(settings.source == SourceType::LEFT);
	CHECK(settings.progressColour == defaultProgressColour(SlotId::FOUR));
	CHECK(settings.playback == PlaybackMode::LOOP);
}

TEST(OperationalLoopStationModel, CopiesConfigurationWithoutReplacingDestinationBinding) {
	SongModel model;
	auto& source = model.tracks[1].settings;
	source.source = SourceType::STEREO;
	source.sourceTrackId = 17;
	source.monitoring = false;
	source.durationBars = 8;
	source.progressColour = PadColour::PINK;
	source.playback = PlaybackMode::ONE_SHOT;
	source.transpose = -7;
	source.transposeCents = 23;
	model.tracks[5].clipSlotId = 99;

	CHECK_TRUE(copyTrackConfiguration(model, SlotId::TWO, SlotId::SIX));
	auto const& destination = model.tracks[5].settings;
	CHECK(destination.source == source.source);
	CHECK_EQUAL(source.sourceTrackId, destination.sourceTrackId);
	CHECK_EQUAL(source.monitoring, destination.monitoring);
	CHECK_EQUAL(source.durationBars, destination.durationBars);
	CHECK(destination.progressColour == source.progressColour);
	CHECK(destination.playback == source.playback);
	CHECK_EQUAL(source.transpose, destination.transpose);
	CHECK_EQUAL(source.transposeCents, destination.transposeCents);
	CHECK_EQUAL(99, model.tracks[5].clipSlotId);
	CHECK_FALSE(copyTrackConfiguration(model, SlotId::TWO, SlotId::TWO));
}

TEST(OperationalLoopStationModel, ProgressColoursExcludeReservedStateColours) {
	SongModel model;
	CHECK_EQUAL(11, kProgressColours.size());
	for (auto colour : kProgressColours) {
		CHECK_FALSE(isReservedStateColour(colour));
	}
	constexpr std::array reserved{PadColour::GREEN, PadColour::RED, PadColour::ORANGE};
	for (size_t i = 0; i < reserved.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		writeTrackParameter(model, slot, TrackParameter::PROGRESS_COLOUR, static_cast<int32_t>(reserved[i]));
		CHECK(model.tracks[i].settings.progressColour == defaultProgressColour(slot));
		CHECK_FALSE(isReservedStateColour(model.tracks[i].settings.progressColour));
	}

	model.tracks[7].settings.progressColour = PadColour::GREEN;
	model.sanitize();
	CHECK(model.tracks[7].settings.progressColour == defaultProgressColour(SlotId::EIGHT));

	writeTrackParameter(model, SlotId::ONE, TrackParameter::PROGRESS_COLOUR, static_cast<int32_t>(PadColour::PINK));
	CHECK(model.tracks[0].settings.progressColour == PadColour::PINK);
}

TEST(OperationalLoopStationModel, TrackMenuParametersAreScopedAndClamped) {
	SongModel model;
	constexpr SlotId slot = SlotId::FOUR;
	writeTrackParameter(model, slot, TrackParameter::SOURCE, static_cast<int32_t>(SourceType::BALANCED));
	writeTrackParameter(model, slot, TrackParameter::SOURCE_TRACK, 7);
	writeTrackParameter(model, slot, TrackParameter::MONITORING, 0);
	writeTrackParameter(model, slot, TrackParameter::DURATION_BARS, 12);
	writeTrackParameter(model, slot, TrackParameter::PROGRESS_COLOUR, static_cast<int32_t>(PadColour::PURPLE));
	writeTrackParameter(model, slot, TrackParameter::PLAYBACK, static_cast<int32_t>(PlaybackMode::ONE_SHOT));
	writeTrackParameter(model, slot, TrackParameter::TRANSPOSE, 3000);

	CHECK_EQUAL(static_cast<int32_t>(SourceType::BALANCED), readTrackParameter(model, slot, TrackParameter::SOURCE));
	CHECK_EQUAL(7, readTrackParameter(model, slot, TrackParameter::SOURCE_TRACK));
	CHECK_EQUAL(0, readTrackParameter(model, slot, TrackParameter::MONITORING));
	CHECK_EQUAL(12, readTrackParameter(model, slot, TrackParameter::DURATION_BARS));
	CHECK_EQUAL(static_cast<int32_t>(PadColour::PURPLE),
	            readTrackParameter(model, slot, TrackParameter::PROGRESS_COLOUR));
	CHECK_EQUAL(static_cast<int32_t>(PlaybackMode::ONE_SHOT),
	            readTrackParameter(model, slot, TrackParameter::PLAYBACK));
	CHECK_EQUAL(2450, readTrackParameter(model, slot, TrackParameter::TRANSPOSE));
	CHECK(model.tracks[0].settings.source == SourceType::LEFT);

	writeTrackParameter(model, slot, TrackParameter::SOURCE_TRACK, -5);
	writeTrackParameter(model, slot, TrackParameter::DURATION_BARS, 999999);
	writeTrackParameter(model, slot, TrackParameter::SOURCE, static_cast<int32_t>(SourceType::MIX_PRE_FX));
	writeTrackParameter(model, slot, TrackParameter::TRANSPOSE, -3000);
	CHECK_EQUAL(0, readTrackParameter(model, slot, TrackParameter::SOURCE_TRACK));
	CHECK_EQUAL(std::numeric_limits<uint16_t>::max(), readTrackParameter(model, slot, TrackParameter::DURATION_BARS));
	CHECK_EQUAL(static_cast<int32_t>(SourceType::LEFT), readTrackParameter(model, slot, TrackParameter::SOURCE));
	CHECK_EQUAL(-2450, readTrackParameter(model, slot, TrackParameter::TRANSPOSE));
}

TEST_GROUP(LoopStationGeometry){};

TEST(LoopStationGeometry, MapsEightFourByFourCells) {
	// Matrix Y increases upward: tracks 1-4 occupy the physical top row.
	CHECK((Geometry::origin(SlotId::ONE) == PadCoordinate{0, 4}));
	CHECK((Geometry::origin(SlotId::FOUR) == PadCoordinate{12, 4}));
	CHECK((Geometry::origin(SlotId::FIVE) == PadCoordinate{0, 0}));
	CHECK((Geometry::origin(SlotId::EIGHT) == PadCoordinate{12, 0}));
	CHECK(Geometry::slotAt(0, 7) == SlotId::ONE);
	CHECK(Geometry::slotAt(15, 7) == SlotId::FOUR);
	CHECK(Geometry::slotAt(0, 0) == SlotId::FIVE);
	CHECK(Geometry::slotAt(15, 0) == SlotId::EIGHT);
	CHECK_TRUE(Geometry::isCentre(1, 1));
	CHECK_TRUE(Geometry::isCentre(2, 2));
	CHECK_FALSE(Geometry::isCentre(0, 1));
}

TEST(LoopStationGeometry, BorderIsClockwiseFromTopLeft) {
	// Pad-matrix Y coordinates increase upward on the physical Deluge.
	PadCoordinate expected[]{{0, 3}, {1, 3}, {2, 3}, {3, 3}, {3, 2}, {3, 1},
	                         {3, 0}, {2, 0}, {1, 0}, {0, 0}, {0, 1}, {0, 2}};
	for (size_t i = 0; i < kCellBorderPadCount; ++i) {
		CHECK(Geometry::kClockwiseBorder[i] == expected[i]);
	}
	CHECK_EQUAL(0, Geometry::progressPadCount(0, 120));
	CHECK_EQUAL(6, Geometry::progressPadCount(60, 120));
	CHECK_EQUAL(11, Geometry::progressPadCount(119, 120));
}

TEST(LoopStationGeometry, ProgressFadesEachClockwisePadInContinuously) {
	auto start = Geometry::progressFill(0, 120);
	CHECK_EQUAL(0, start.fullPads);
	CHECK_EQUAL(0, start.leadingBrightness);
	auto withinFirstTick = Geometry::progressFill(static_cast<uint64_t>(1) << 15, 120);
	CHECK_EQUAL(0, withinFirstTick.fullPads);
	CHECK_TRUE(withinFirstTick.leadingBrightness > 0);
	CHECK_TRUE(withinFirstTick.leadingBrightness
	           < Geometry::progressFill(static_cast<uint64_t>(1) << 16, 120).leadingBrightness);

	auto halfwayThroughFirstPad = Geometry::progressFill(static_cast<uint64_t>(5) << 16, 120);
	CHECK_EQUAL(0, halfwayThroughFirstPad.fullPads);
	CHECK_EQUAL(32768, halfwayThroughFirstPad.leadingBrightness);

	auto secondPad = Geometry::progressFill(static_cast<uint64_t>(10) << 16, 120);
	CHECK_EQUAL(1, secondPad.fullPads);
	CHECK_EQUAL(0, secondPad.leadingBrightness);

	auto halfwayThroughLastPad = Geometry::progressFill(static_cast<uint64_t>(115) << 16, 120);
	CHECK_EQUAL(11, halfwayThroughLastPad.fullPads);
	CHECK_EQUAL(32768, halfwayThroughLastPad.leadingBrightness);

	auto wrapped = Geometry::progressFill(static_cast<uint64_t>(120) << 16, 120);
	CHECK_EQUAL(0, wrapped.fullPads);
	CHECK_EQUAL(0, wrapped.leadingBrightness);
}

TEST(LoopStationGeometry, DeleteHoldProgressFillsOnceAndDoesNotWrap) {
	constexpr uint32_t duration = 88200;
	auto start = Geometry::linearProgressFill(0, duration);
	CHECK_EQUAL(0, start.fullPads);
	CHECK_EQUAL(0, start.leadingBrightness);

	auto halfway = Geometry::linearProgressFill(duration / 2, duration);
	CHECK_EQUAL(6, halfway.fullPads);
	CHECK_EQUAL(0, halfway.leadingBrightness);

	auto complete = Geometry::linearProgressFill(duration, duration);
	CHECK_EQUAL(kCellBorderPadCount, complete.fullPads);
	CHECK_EQUAL(0, complete.leadingBrightness);

	auto heldLonger = Geometry::linearProgressFill(duration * 2, duration);
	CHECK_EQUAL(kCellBorderPadCount, heldLonger.fullPads);
	CHECK_EQUAL(0, heldLonger.leadingBrightness);
}

TEST(LoopStationGeometry, BeatPulseIsContinuousAndPeaksOnTempoBoundary) {
	constexpr uint32_t beatLength = 96;
	CHECK_EQUAL(65535, Geometry::beatPulseBrightness(0, beatLength));
	CHECK_EQUAL(32767, Geometry::beatPulseBrightness(static_cast<uint64_t>(24) << 16, beatLength));
	CHECK_EQUAL(0, Geometry::beatPulseBrightness(static_cast<uint64_t>(48) << 16, beatLength));
	CHECK_EQUAL(32767, Geometry::beatPulseBrightness(static_cast<uint64_t>(72) << 16, beatLength));
	CHECK_EQUAL(65535, Geometry::beatPulseBrightness(static_cast<uint64_t>(beatLength) << 16, beatLength));
}

TEST_GROUP(LoopStationQuantize){};

TEST_GROUP(LoopStationState){};

TEST(LoopStationState, PerformanceGesturesActOnFirstPadDown) {
	CHECK_TRUE(centreGestureActsOnPress(TrackState::EMPTY, false));
	CHECK_TRUE(centreGestureActsOnPress(TrackState::RECORDING, false));
	CHECK_TRUE(centreGestureActsOnPress(TrackState::PLAYING, false));
	CHECK_TRUE(centreGestureActsOnPress(TrackState::OVERDUBBING, false));
	CHECK_FALSE(centreGestureActsOnPress(TrackState::STOPPED, false));
	CHECK_FALSE(centreGestureActsOnPress(TrackState::PLAYING, true));
}

TEST(LoopStationState, RecordOverdubPlayOrder) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_OVERDUB_PLAY;
	Engine engine(model);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	CHECK(engine.track(SlotId::ONE).state == TrackState::RECORDING);
	engine.centrePressed(SlotId::ONE, at(70), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	CHECK_EQUAL(70, engine.track(SlotId::ONE).phraseLength);
	CHECK_EQUAL(UINT32_MAX, engine.ticksUntilNextEvent(70));
	engine.processTick(at(139));
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	finishDub(engine, 140);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_EQUAL(70, engine.track(SlotId::ONE).phraseLength);
}

TEST(LoopStationState, RecordPlayOverdubOrder) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	Engine engine(model);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::ONE, at(70), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	engine.centrePressed(SlotId::ONE, at(80), 2000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	CHECK_FALSE(engine.track(SlotId::ONE).event.pending());
	CHECK_EQUAL(UINT32_MAX, engine.ticksUntilNextEvent(80));
	engine.processTick(at(149));
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	finishDub(engine, 150);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_EQUAL(70, engine.track(SlotId::ONE).phraseLength);
}

TEST(LoopStationState, FixedRecordDurationNeverChangesOverdubLength) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	model.tracks[0].settings.durationBars = 4;
	Delegate delegate;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 70, true);
	engine.centrePressed(SlotId::ONE, at(80), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	CHECK_FALSE(engine.track(SlotId::ONE).event.pending());
	CHECK_EQUAL(UINT32_MAX, engine.ticksUntilNextEvent(80));
	finishDub(engine, 150);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_EQUAL(70, engine.track(SlotId::ONE).phraseLength);
	CHECK_EQUAL(70, delegate.finishLengths[0]);
	CHECK_TRUE(delegate.finishOverdubs[0]);
}

TEST(LoopStationState, FixedLengthCompletesAtExactBars) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	model.tracks[0].settings.durationBars = 2;
	Engine engine(model);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.processTick(at(1));
	CHECK_TRUE(engine.track(SlotId::ONE).automaticEndTick != 0);
	engine.processTick(at(191));
	CHECK(engine.track(SlotId::ONE).state == TrackState::RECORDING);
	engine.processTick(at(192));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_EQUAL(192, engine.track(SlotId::ONE).phraseLength);
}

TEST(LoopStationState, DoubleTapDuringOverdubStops) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;

	Delegate delegate;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::ONE, at(48), 1000);
	model.global.stopAction = {ActionMode::SYNCED, ActionDivision::BAR};
	// Start overdub at next phrase boundary (96)
	engine.centrePressed(SlotId::ONE, at(96), 2000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	// Double tap aborts overdub and stops
	engine.centrePressed(SlotId::ONE, at(97), 2200);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK(engine.track(SlotId::ONE).event.action == PendingAction::STOP);
	engine.processTick(at(144));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	engine.processTick(at(192));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	CHECK_EQUAL(1, delegate.aborts[0]);
}

TEST(LoopStationState, QuickSkipOfAutomaticOverdubPreservesInitialRecording) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_OVERDUB_PLAY;
	Delegate delegate;
	Engine engine(model, &delegate);

	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::ONE, at(48), 1000);
	// Recording finishes, auto-overdub scheduled at next phrase boundary (96)
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	CHECK_EQUAL(1, delegate.finishes[0]);
	auto initialRevision = engine.track(SlotId::ONE).currentRevision;

	// Skip the auto-overdub before it begins (at tick 49, before 96)
	engine.centrePressed(SlotId::ONE, at(49), 1200);
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	engine.processTick(at(96));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	CHECK_EQUAL(1, delegate.aborts[0]);
	CHECK_EQUAL(1, delegate.finishes[0]);
	CHECK_EQUAL(initialRevision, engine.track(SlotId::ONE).currentRevision);
	CHECK_TRUE(engine.track(SlotId::ONE).undoAvailable);
}

TEST(LoopStationState, UndoRedoIsIndependentPerTrack) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	Engine engine(model);
	for (auto slot : {SlotId::ONE, SlotId::TWO}) {
		engine.centrePressed(slot, at(0), 0);
		engine.centrePressed(slot, at(48), 1000);
		engine.centrePressed(slot, at(48), 2000);
	}
	finishDub(engine, 96);
	finishDub(engine, 96, SlotId::TWO);
	CHECK_TRUE(engine.undo(SlotId::ONE));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK(engine.track(SlotId::TWO).state == TrackState::PLAYING);
	CHECK_TRUE(engine.track(SlotId::ONE).redoAvailable);
	CHECK_TRUE(engine.redo(SlotId::ONE));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
}

TEST(LoopStationState, FailedBackingSnapshotSwapLeavesUndoStateUntouched) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	Delegate delegate;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::ONE, at(48), 1000);
	engine.centrePressed(SlotId::ONE, at(48), 2000);
	finishDub(engine, 96);
	auto revision = engine.track(SlotId::ONE).currentRevision;
	delegate.rejectSwap = true;
	engine.undo(SlotId::ONE);
	CHECK_EQUAL(1, delegate.swaps[0]);
	CHECK_EQUAL(revision, engine.track(SlotId::ONE).currentRevision);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_TRUE(engine.track(SlotId::ONE).undoAvailable);
	CHECK_FALSE(engine.track(SlotId::ONE).redoAvailable);
}

TEST(LoopStationState, InitialTakeAndLatestOverdubCanBothBeUndoneAndRedone) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	Delegate delegate;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::ONE, at(48), 1000);
	CHECK_TRUE(engine.undo(SlotId::ONE));
	CHECK_EQUAL(1, delegate.swaps[0]);
	CHECK_FALSE(delegate.swapTargetsHavePhrase[0]);
	CHECK(engine.track(SlotId::ONE).state == TrackState::EMPTY);
	CHECK_TRUE(engine.track(SlotId::ONE).redoAvailable);
	CHECK_TRUE(engine.redo(SlotId::ONE));
	CHECK_EQUAL(2, delegate.swaps[0]);
	CHECK_TRUE(delegate.swapTargetsHavePhrase[0]);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_TRUE(engine.track(SlotId::ONE).undoAvailable);

	engine.centrePressed(SlotId::ONE, at(48), 2000);
	finishDub(engine, 96);
	CHECK_TRUE(engine.undo(SlotId::ONE));
	CHECK_EQUAL(3, delegate.swaps[0]);
	CHECK_TRUE(delegate.swapTargetsHavePhrase[0]);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_TRUE(engine.undo(SlotId::ONE));
	CHECK_EQUAL(4, delegate.swaps[0]);
	CHECK_FALSE(delegate.swapTargetsHavePhrase[0]);
	CHECK(engine.track(SlotId::ONE).state == TrackState::EMPTY);
	CHECK_TRUE(engine.redo(SlotId::ONE));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_TRUE(engine.redo(SlotId::ONE));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
}

TEST(LoopStationState, RotatedOverdubSampleMapsBackToLogicalLoopPhase) {
	constexpr uint32_t length = 96;
	constexpr uint32_t overdubStartPhase = 24;
	CHECK_EQUAL(0, samplePhaseForLoopPhase(24, overdubStartPhase, length));
	CHECK_EQUAL(72, samplePhaseForLoopPhase(0, overdubStartPhase, length));
	CHECK_EQUAL(95, samplePhaseForLoopPhase(23, overdubStartPhase, length));
	CHECK_EQUAL(24, samplePhaseForLoopPhase(48, overdubStartPhase, length));
}

TEST(LoopStationState, ClearRetainsBindingAndSettings) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.tracks[0].clipSlotId = 1;
	Engine engine(model);
	engine.restoreBoundPhrase(SlotId::ONE, 96, false);
	engine.clearHeld(SlotId::ONE);
	CHECK(engine.track(SlotId::ONE).state == TrackState::EMPTY);
	CHECK_EQUAL(1, model.tracks[0].clipSlotId);
	CHECK_FALSE(engine.track(SlotId::ONE).undoAvailable);
}

TEST(LoopStationState, EightEventsCompleteIndependently) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.startAction = {ActionMode::SYNCED, ActionDivision::BAR};
	model.global.stopAction = {ActionMode::SYNCED, ActionDivision::BAR};
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	Engine engine(model);
	for (size_t i = 0; i < kTrackCount; ++i) {
		engine.centrePressed(static_cast<SlotId>(i), at(10), i * 1000);
		CHECK_TRUE(engine.track(static_cast<SlotId>(i)).event.pending());
	}
	CHECK_EQUAL(86, engine.ticksUntilNextEvent(10));
	engine.processTick(at(96));
	for (size_t i = 0; i < kTrackCount; ++i) {
		CHECK(engine.track(static_cast<SlotId>(i)).state == TrackState::RECORDING);
	}
	engine.centrePressed(SlotId::THREE, at(100), 9000);
	engine.processTick(at(192));
	CHECK(engine.track(SlotId::THREE).state == TrackState::PLAYING);
	CHECK(engine.track(SlotId::TWO).state == TrackState::RECORDING);
}

TEST(LoopStationState, ExitWaitsForAsynchronousCommit) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	CHECK_FALSE(engine.requestExit(at(48)));
	CHECK(engine.track(SlotId::ONE).committingLength != 0);
	CHECK_FALSE(engine.exitReady());
	engine.notifyCommitComplete(SlotId::ONE, true);
	CHECK_TRUE(engine.exitReady());
	CHECK_EQUAL(48, engine.track(SlotId::ONE).phraseLength);
}

TEST(LoopStationState, DependentOverdubStartsAfterAsyncCommitCompletes) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_OVERDUB_PLAY;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::ONE, at(48), 1000);
	CHECK(engine.track(SlotId::ONE).committingLength != 0);

	engine.notifyCommitComplete(SlotId::ONE, true, Error::NONE, 60);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	CHECK_EQUAL(2, delegate.begins[0]);
	CHECK_EQUAL(48, delegate.beginTicks[0]);
	CHECK_EQUAL(0, engine.track(SlotId::ONE).overdubEndTick);
}

TEST(LoopStationState, ExitFromFirstTakeDoesNotEnterConfiguredOverdub) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Engine engine(model);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	CHECK_TRUE(engine.requestExit(at(48)));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
}

TEST(LoopStationState, ExitCancelsPendingMeasureStart) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.startAction = {ActionMode::SYNCED, ActionDivision::BAR};
	model.global.stopAction = {ActionMode::SYNCED, ActionDivision::BAR};
	Engine engine(model);
	engine.centrePressed(SlotId::ONE, at(10), 0);
	CHECK_TRUE(engine.track(SlotId::ONE).event.pending());
	CHECK_TRUE(engine.requestExit(at(20)));
	CHECK_FALSE(engine.track(SlotId::ONE).event.pending());
	CHECK(engine.track(SlotId::ONE).state == TrackState::EMPTY);
}

TEST(LoopStationState, FailedPreflightOnlyRejectsAffectedSlot) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	Engine engine(model, &delegate);
	delegate.reject = true;
	engine.centrePressed(SlotId::ONE, at(0), 0);
	delegate.reject = false;
	engine.centrePressed(SlotId::TWO, at(0), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::ERROR);
	CHECK(engine.track(SlotId::TWO).state == TrackState::RECORDING);
}

TEST(LoopStationState, CentrePressAcknowledgesErrorWithoutStartingAnotherAction) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	Engine engine(model, &delegate);
	delegate.reject = true;
	engine.centrePressed(SlotId::ONE, at(0), 0);
	CHECK(engine.track(SlotId::ONE).state == TrackState::ERROR);
	delegate.reject = false;
	engine.centrePressed(SlotId::ONE, at(1), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::EMPTY);
	CHECK_EQUAL(1, delegate.preflights[0]);
}

TEST(LoopStationState, NewCompletedTakeInvalidatesRedo) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	Engine engine(model);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::ONE, at(48), 1000);
	engine.centrePressed(SlotId::ONE, at(48), 2000);
	finishDub(engine, 96);
	CHECK_TRUE(engine.undo(SlotId::ONE));
	CHECK_TRUE(engine.track(SlotId::ONE).redoAvailable);
	engine.centrePressed(SlotId::ONE, at(96), 3000);
	finishDub(engine, 144);
	CHECK_TRUE(engine.track(SlotId::ONE).undoAvailable);
	CHECK_FALSE(engine.track(SlotId::ONE).redoAvailable);
}

TEST(LoopStationState, DelegateMayCorrectCommittedFreshTakeLength) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	delegate.committedLength = 47;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, at(12), 0);
	engine.centrePressed(SlotId::ONE, at(60), 1000);
	CHECK_EQUAL(48, delegate.finishLengths[0]);
	CHECK_EQUAL(47, engine.track(SlotId::ONE).phraseLength);
	CHECK_TRUE(engine.track(SlotId::ONE).undoAvailable);
}

TEST(LoopStationState, FixedDurationClampsToMaximumSequenceLength) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.tracks[0].settings.durationBars = std::numeric_limits<uint16_t>::max();
	Engine engine(model);
	engine.centrePressed(SlotId::ONE, at(0, 100000), 0);
	engine.processTick(at(1, 100000));
	CHECK_TRUE(engine.track(SlotId::ONE).automaticEndTick != 0);
	CHECK_EQUAL(kMaxLoopSequenceLength, engine.track(SlotId::ONE).automaticEndTick);
}

TEST(LoopStationState, DelegateReceivesCaptureKindTickAndLength) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	Delegate delegate;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::THREE, at(12), 0);
	engine.centrePressed(SlotId::THREE, at(60), 1000);
	CHECK_EQUAL(1, delegate.preflights[2]);
	CHECK_EQUAL(1, delegate.begins[2]);
	CHECK_EQUAL(12, delegate.beginTicks[2]);
	CHECK_FALSE(delegate.beginOverdubs[2]);
	CHECK_EQUAL(48, delegate.finishLengths[2]);
	engine.centrePressed(SlotId::THREE, at(72), 2000);
	CHECK_FALSE(engine.track(SlotId::THREE).event.pending());
	CHECK_TRUE(delegate.beginOverdubs[2]);
	CHECK_EQUAL(72, delegate.beginTicks[2]);
	CHECK_EQUAL(UINT32_MAX, engine.ticksUntilNextEvent(72));
	finishDub(engine, 120, SlotId::THREE);
	CHECK_TRUE(delegate.finishOverdubs[2]);
	CHECK_EQUAL(48, engine.track(SlotId::THREE).phraseLength);
}

TEST(LoopStationState, ClearNotifiesDelegateAndResetKeepsSongSettings) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, false);
	engine.clearHeld(SlotId::ONE);
	CHECK_EQUAL(1, delegate.clears[0]);
	engine.resetRuntime();
	CHECK(engine.track(SlotId::ONE).state == TrackState::EMPTY);
}

TEST(LoopStationState, StartAllUsesOneSharedQuantizedBoundary) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.startAction.mode = ActionMode::IMMEDIATE; // Recording quantization does not control playback timing.
	model.tracks[1].settings.playback = PlaybackMode::ONE_SHOT;
	Delegate delegate;
	model.global.startAction.division = model.global.stopAction.division = ActionDivision::BAR;
	model.global.startAction.mode = model.global.stopAction.mode = ActionMode::SYNCED;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, false);
	engine.restoreBoundPhrase(SlotId::TWO, 96, false);
	CHECK_TRUE(engine.canStartAll());

	engine.startAll(at(20));
	CHECK(engine.track(SlotId::ONE).event.action == PendingAction::RETRIGGER);
	CHECK(engine.track(SlotId::TWO).event.action == PendingAction::RETRIGGER);
	CHECK_EQUAL(96, engine.track(SlotId::ONE).event.tick);
	CHECK_EQUAL(96, engine.track(SlotId::TWO).event.tick);
	CHECK_FALSE(engine.canStartAll());

	engine.processTick(at(96));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK(engine.track(SlotId::TWO).state == TrackState::PLAYING);
	CHECK_EQUAL(96, engine.track(SlotId::TWO).playbackStartTick);
}

TEST(LoopStationState, HoldClearRestoresOutputGainBeforeNextRecording) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, false, true, 1, 0);
	CHECK_EQUAL(0, delegate.lastGains[0]);

	engine.clearHeld(SlotId::ONE);
	CHECK(engine.track(SlotId::ONE).state == TrackState::EMPTY);
	CHECK_EQUAL(1 << 24, engine.track(SlotId::ONE).playbackGain);
	CHECK_EQUAL(1 << 24, delegate.lastGains[0]);

	engine.centrePressed(SlotId::ONE, at(10), 1000);
	engine.centrePressed(SlotId::ONE, at(58), 2000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_EQUAL(1 << 24, delegate.lastGains[0]);
}

TEST(LoopStationState, UndoToEmptyRestoresOutputGainBeforeNextRecording) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, false, true, 1, 0);
	CHECK_EQUAL(0, delegate.lastGains[0]);

	CHECK_TRUE(engine.undo(SlotId::ONE));
	CHECK(engine.track(SlotId::ONE).state == TrackState::EMPTY);
	CHECK_EQUAL(1 << 24, engine.track(SlotId::ONE).playbackGain);
	CHECK_EQUAL(1 << 24, delegate.lastGains[0]);

	engine.centrePressed(SlotId::ONE, at(10), 1000);
	engine.centrePressed(SlotId::ONE, at(58), 2000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_EQUAL(1 << 24, delegate.lastGains[0]);
}

TEST(LoopStationState, UndoFromStoppedMakesEarlierRevisionAudible) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, false, true, 2, 1);
	CHECK_EQUAL(0, delegate.lastGains[0]);

	CHECK_TRUE(engine.undo(SlotId::ONE));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_EQUAL(1 << 24, engine.track(SlotId::ONE).playbackGain);
	CHECK_EQUAL(1 << 24, delegate.lastGains[0]);
}

TEST(LoopStationState, MultipleAsynchronousCommitsBlockExitIndependently) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::TWO, at(0), 0);
	CHECK_FALSE(engine.requestExit(at(48)));
	CHECK_TRUE(engine.anyCapturing());
	engine.notifyCommitComplete(SlotId::TWO, true);
	CHECK_FALSE(engine.exitReady());
	engine.notifyCommitComplete(SlotId::ONE, true);
	CHECK_TRUE(engine.exitReady());
	CHECK_FALSE(engine.anyCapturing());
}

TEST(LoopStationState, FailedCommitPreservesPreviousPhraseRevision) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true);
	auto revision = engine.track(SlotId::ONE).currentRevision;
	engine.centrePressed(SlotId::ONE, at(96), 0);
	finishDub(engine, 192);
	CHECK(engine.track(SlotId::ONE).committingLength != 0);
	engine.notifyCommitComplete(SlotId::ONE, false, Error::SD_CARD);
	CHECK(engine.track(SlotId::ONE).state == TrackState::ERROR);
	CHECK(engine.track(SlotId::ONE).stateBeforeCommit == TrackState::PLAYING);
	CHECK_EQUAL(revision, engine.track(SlotId::ONE).currentRevision);
	CHECK_EQUAL(96, engine.track(SlotId::ONE).phraseLength);
}

TEST(LoopStationState, AsyncCommitPreservesSpecificStorageFailure) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::ONE, at(48), 1000);
	engine.notifyCommitComplete(SlotId::ONE, false, Error::SD_CARD_FULL);
	CHECK(engine.track(SlotId::ONE).state == TrackState::ERROR);
	CHECK(engine.track(SlotId::ONE).error == Error::SD_CARD_FULL);
}

TEST(LoopStationState, SubTickTakeRetainsPositiveClipMetadata) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::ONE, at(0), 10);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_EQUAL(0, delegate.aborts[0]);
	CHECK_EQUAL(1, delegate.finishes[0]);
	CHECK_EQUAL(1, engine.track(SlotId::ONE).historySize);
	CHECK_EQUAL(1, engine.track(SlotId::ONE).phraseLength);
}

TEST(LoopStationState, OneTickTakeCommitsOnceDespiteRepeatedCompletion) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::ONE, at(1), 20);
	engine.notifyCommitComplete(SlotId::ONE, true);
	engine.notifyCommitComplete(SlotId::ONE, true);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_EQUAL(1, engine.track(SlotId::ONE).phraseLength);
	CHECK_EQUAL(1, engine.track(SlotId::ONE).historySize);
	CHECK_EQUAL(1, delegate.finishes[0]);
}

TEST(LoopStationState, FinishDelegateCannotBeReenteredWhileYieldingToAudioOrPads) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.tracks[0].settings.durationBars = 1;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	delegate.duringFinish = [&] {
		CHECK(engine.track(SlotId::ONE).finishingCapture);
		engine.processTick(at(96));
		engine.centrePressed(SlotId::ONE, at(96), 1000);
	};
	engine.processTick(at(96));
	CHECK_EQUAL(1, delegate.finishes[0]);
	CHECK_FALSE(engine.track(SlotId::ONE).finishingCapture);
	CHECK(engine.track(SlotId::ONE).committingLength != 0);
	engine.notifyCommitComplete(SlotId::ONE, true);
	CHECK_EQUAL(1, engine.track(SlotId::ONE).historySize);
}

TEST(LoopStationState, FailedShortOverdubPreservesRedoAndAcknowledgesWithoutRecording) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 1, true, true, 3, 1);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	finishDub(engine, 1);
	engine.notifyCommitComplete(SlotId::ONE, false, Error::ALLOCATION);
	engine.notifyCommitComplete(SlotId::ONE, false, Error::SD_CARD);
	CHECK(engine.track(SlotId::ONE).error == Error::ALLOCATION);
	CHECK_EQUAL(3, engine.track(SlotId::ONE).historySize);
	CHECK_EQUAL(1, engine.track(SlotId::ONE).historyCursor);
	CHECK(engine.track(SlotId::ONE).redoAvailable);
	engine.centrePressed(SlotId::ONE, at(2), 40);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_EQUAL(1, delegate.begins[0]);
}

TEST(LoopStationState, BackgroundOverdubCannotFinishAgainWhileCommitIsPending) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	engine.centrePressed(SlotId::ONE, at(48), 1000);
	CHECK_FALSE(engine.track(SlotId::ONE).finishingOverdub);
	engine.serviceAudioCompletion(at(96));
	for (uint32_t tick = 97; tick < 200; ++tick) {
		engine.processTick(at(tick));
	}
	CHECK_EQUAL(1, delegate.finishes[0]);
	CHECK(engine.track(SlotId::ONE).committingLength != 0);
	CHECK_FALSE(engine.requestExit(at(200)));
	CHECK_FALSE(engine.track(SlotId::ONE).event.pending());
	engine.notifyCommitComplete(SlotId::ONE, true);
	CHECK_FALSE(engine.track(SlotId::ONE).finishingOverdub);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK(engine.exitReady());
}

TEST(LoopStationState, RejectedBackingReportsSpecificErrorAndPreservesPhrase) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	delegate.rejectFinish = true;
	delegate.rejectError = Error::BACKING_UNAVAILABLE;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true);
	auto revision = engine.track(SlotId::ONE).currentRevision;
	engine.centrePressed(SlotId::ONE, at(0), 0);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	finishDub(engine, 96);
	CHECK(engine.track(SlotId::ONE).state == TrackState::ERROR);
	CHECK(engine.track(SlotId::ONE).error == Error::BACKING_UNAVAILABLE);
	CHECK_EQUAL(revision, engine.track(SlotId::ONE).currentRevision);
	CHECK_EQUAL(96, engine.track(SlotId::ONE).phraseLength);
	engine.centrePressed(SlotId::ONE, at(97), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
}

TEST(LoopStationState, InternalFinalizationFailurePreservesCommittedHistoryAndAllowsAcknowledgement) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true, true, 3, 1);
	engine.centrePressed(SlotId::ONE, at(0), 0);
	finishDub(engine, 96);
	CHECK(engine.track(SlotId::ONE).committingLength != 0);
	engine.notifyCommitComplete(SlotId::ONE, false, Error::INTERNAL);
	CHECK(engine.track(SlotId::ONE).state == TrackState::ERROR);
	CHECK(engine.track(SlotId::ONE).error == Error::INTERNAL);
	CHECK_EQUAL(96, engine.track(SlotId::ONE).phraseLength);
	CHECK_EQUAL(3, engine.track(SlotId::ONE).historySize);
	CHECK_EQUAL(1, engine.track(SlotId::ONE).historyCursor);
	CHECK(engine.track(SlotId::ONE).redoAvailable);
	engine.centrePressed(SlotId::ONE, at(97), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_EQUAL(1, delegate.begins[0]);
}

TEST(LoopStationState, IndependentStartAndStopTimingForTapsAndGroupButtons) {
	for (auto start : {ActionMode::IMMEDIATE, ActionMode::SYNCED}) {
		for (auto stop : {ActionMode::IMMEDIATE, ActionMode::SYNCED}) {
			for (bool group : {false, true}) {
				SongModel model;

				model.global.startAction.mode = start;
				model.global.stopAction.mode = stop;
				model.global.startAction.division = model.global.stopAction.division = ActionDivision::BAR;
				Engine engine(model);
				engine.restoreBoundPhrase(SlotId::ONE, 144, false);
				if (group)
					engine.startAll(at(20));
				else
					engine.centrePressed(SlotId::ONE, at(20), 1000);
				CHECK(engine.track(SlotId::ONE).state
				      == (start == ActionMode::IMMEDIATE ? TrackState::PLAYING : TrackState::STOPPED));
				engine.processTick(at(96));
				CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
				if (group)
					engine.stopAll(at(102));
				else {
					engine.centrePressed(SlotId::ONE, at(101), 2000);
					engine.centrePressed(SlotId::ONE, at(102), 2100);
				}
				CHECK(engine.track(SlotId::ONE).state
				      == (stop == ActionMode::IMMEDIATE ? TrackState::STOPPED : TrackState::PLAYING));
				if (stop == ActionMode::SYNCED)
					CHECK_EQUAL(192, engine.track(SlotId::ONE).event.tick);
				engine.processTick(at(192));
				CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
			}
		}
	}
}

TEST(LoopStationState, ImmediatePlaybackKeepsSingleStartAndDoubleStopGestures) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.stopAction.mode = ActionMode::IMMEDIATE;

	Delegate delegate;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 144, false);
	engine.centrePressed(SlotId::ONE, at(20), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	CHECK_FALSE(engine.track(SlotId::ONE).event.pending());
	CHECK_EQUAL(0, delegate.begins[0]);
	engine.centrePressed(SlotId::ONE, at(21), 2000);
	engine.centrePressed(SlotId::ONE, at(22), 2100);
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	CHECK_FALSE(engine.track(SlotId::ONE).event.pending());
	CHECK_EQUAL(1, delegate.aborts[0]);
	CHECK_EQUAL(0, engine.track(SlotId::ONE).playbackGain);
	// Initial recording uses the same immediate start action.
	engine.centrePressed(SlotId::TWO, at(23), 3000);
	CHECK(engine.track(SlotId::TWO).state == TrackState::RECORDING);
	CHECK_FALSE(engine.track(SlotId::TWO).event.pending());
}

TEST(LoopStationState, SyncedStartAndDoubleStopUseBarsNotPhraseEnds) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	model.global.startAction.division = model.global.stopAction.division = ActionDivision::BAR;
	model.global.startAction.mode = model.global.stopAction.mode = ActionMode::SYNCED;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 144, false);
	engine.centrePressed(SlotId::ONE, at(20), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	CHECK_EQUAL(96, engine.track(SlotId::ONE).event.tick);
	engine.processTick(at(95));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	engine.processTick(at(96));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	engine.centrePressed(SlotId::ONE, at(101), 2000);
	engine.centrePressed(SlotId::ONE, at(102), 2100);
	CHECK_EQUAL(192, engine.track(SlotId::ONE).event.tick);
	engine.processTick(at(144));
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	engine.processTick(at(192));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
}

TEST(LoopStationState, SyncedGroupStopUsesOneBarForDifferentPhraseLengths) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.startAction.division = model.global.stopAction.division = ActionDivision::BAR;
	model.global.startAction.mode = model.global.stopAction.mode = ActionMode::SYNCED;
	Engine engine(model);
	engine.restoreBoundPhrase(SlotId::ONE, 144, true);
	engine.restoreBoundPhrase(SlotId::TWO, 48, true);
	engine.stopAll(at(20));
	CHECK_EQUAL(96, engine.track(SlotId::ONE).event.tick);
	CHECK_EQUAL(96, engine.track(SlotId::TWO).event.tick);
	engine.processTick(at(48));
	CHECK(engine.track(SlotId::TWO).state == TrackState::PLAYING);
	engine.processTick(at(96));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	CHECK(engine.track(SlotId::TWO).state == TrackState::STOPPED);
}

TEST(LoopStationState, MissingBarLengthDoesNotStrandPlaybackActions) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Engine engine(model);
	engine.restoreBoundPhrase(SlotId::ONE, 144, false);
	engine.centrePressed(SlotId::ONE, at(20, 0), 1000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	engine.stopAll(at(21, 0));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
}

TEST_GROUP(LoopStationFanOut){};

TEST(LoopStationState, SchedulerDelayUsesSchedulerOriginNotUiPosition) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	model.global.startAction.division = model.global.stopAction.division = ActionDivision::BAR;
	model.global.startAction.mode = model.global.stopAction.mode = ActionMode::SYNCED;
	Engine engine(model);
	engine.restoreBoundPhrase(SlotId::ONE, 144, false);
	engine.centrePressed(SlotId::ONE, at(20), 1000);
	// Graphics/gesture clock is at 40; scheduler is still processing tick 24.
	CHECK_EQUAL(72, serviceSchedulerTick(engine, at(40), 24, INT32_MAX));
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
	CHECK_EQUAL(96, engine.track(SlotId::ONE).event.tick);
	// A separate metronome/clip event may wake earlier and must be preserved.
	CHECK_EQUAL(4, serviceSchedulerTick(engine, at(40), 24, 4));
}

TEST(LoopStationState, SchedulerCatchupDoesNotExecuteQueuedStartOrStopEarly) {
	for (bool stop : {false, true}) {
		SongModel model;
		model.global.startAction.mode = ActionMode::IMMEDIATE;
		model.global.startAction.division = model.global.stopAction.division = ActionDivision::BAR;
		model.global.startAction.mode = model.global.stopAction.mode = ActionMode::SYNCED;
		Engine engine(model);
		engine.restoreBoundPhrase(SlotId::ONE, 144, stop);
		if (stop)
			engine.stopAll(at(20));
		else
			engine.startAll(at(20));
		// Wall/display time has passed 96, but this callback belongs to tick 24.
		CHECK_EQUAL(72, serviceSchedulerTick(engine, at(100), 24, INT32_MAX));
		CHECK(engine.track(SlotId::ONE).event.pending());
		CHECK(engine.track(SlotId::ONE).state == (stop ? TrackState::PLAYING : TrackState::STOPPED));
		serviceSchedulerTick(engine, at(100), 96, INT32_MAX);
		CHECK_FALSE(engine.track(SlotId::ONE).event.pending());
		CHECK(engine.track(SlotId::ONE).state == (stop ? TrackState::STOPPED : TrackState::PLAYING));
	}
}

TEST(LoopStationState, FractionalBarBoundaryUsesFollowingBarForStartAndStop) {
	for (bool stop : {false, true}) {
		for (auto timing : {ActionMode::SYNCED, ActionMode::IMMEDIATE}) {
			SongModel model;
			model.global.startAction.mode = ActionMode::IMMEDIATE;
			model.global.startAction.mode = model.global.stopAction.mode = timing;
			model.global.startAction.division = model.global.stopAction.division = ActionDivision::BAR;

			Engine engine(model);
			engine.restoreBoundPhrase(SlotId::ONE, 144, stop);
			auto gesture = at(96);
			gesture.afterTickBoundary = true;
			if (stop)
				engine.stopAll(gesture);
			else
				engine.startAll(gesture);
			if (timing == ActionMode::SYNCED) {
				CHECK_EQUAL(192, engine.track(SlotId::ONE).event.tick);
				serviceSchedulerTick(engine, at(97), 97, INT32_MAX);
				CHECK(engine.track(SlotId::ONE).event.pending());
				serviceSchedulerTick(engine, at(192), 192, INT32_MAX);
			}
			CHECK(engine.track(SlotId::ONE).state == (stop ? TrackState::STOPPED : TrackState::PLAYING));
		}
	}
}

TEST(LoopStationState, SchedulerIgnoresCommittingDeadlinesAndDoesNotSpin) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Engine engine(model);
	engine.restoreBoundPhrase(SlotId::ONE, 144, true);
	engine.track(SlotId::ONE).state = TrackState::COMMITTING;
	engine.track(SlotId::ONE).event = {.action = PendingAction::STOP, .tick = 20};
	CHECK_EQUAL(INT32_MAX, serviceSchedulerTick(engine, at(40), 24, INT32_MAX));
}

TEST(LoopStationState, SchedulerTimingCombinationsAcrossBarResolutions) {
	for (uint32_t bar : {24u, 96u, 384u}) {
		for (auto start : {ActionMode::SYNCED, ActionMode::IMMEDIATE}) {
			for (auto stop : {ActionMode::SYNCED, ActionMode::IMMEDIATE}) {
				SongModel model;

				model.global.startAction.mode = start;
				model.global.stopAction.mode = stop;
				model.global.startAction.division = model.global.stopAction.division = ActionDivision::BAR;
				Engine engine(model);
				engine.restoreBoundPhrase(SlotId::ONE, bar * 3 / 2, false);
				engine.centrePressed(SlotId::ONE, at(bar / 4 - 1, bar), 1000);
				serviceSchedulerTick(engine, at(bar + 1, bar), bar / 2, INT32_MAX);
				CHECK(engine.track(SlotId::ONE).state
				      == (start == ActionMode::SYNCED ? TrackState::STOPPED : TrackState::PLAYING));
				serviceSchedulerTick(engine, at(bar + 1, bar), bar, INT32_MAX);
				engine.centrePressed(SlotId::ONE, at(bar + 2, bar), 2000);
				engine.centrePressed(SlotId::ONE, at(bar + 3, bar), 2100);
				serviceSchedulerTick(engine, at(bar * 2 + 1, bar), bar + 4, INT32_MAX);
				CHECK(engine.track(SlotId::ONE).state
				      == (stop == ActionMode::SYNCED ? TrackState::PLAYING : TrackState::STOPPED));
				serviceSchedulerTick(engine, at(bar * 2 + 1, bar), bar * 2, INT32_MAX);
				CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
			}
		}
	}
}

TEST(LoopStationFanOut, SharedSourceRendersOnceAndMonitoringDeduplicates) {
	FanOutRouter router;
	router.subscribe(
	    {.slot = SlotId::ONE, .source = {SourceType::LEFT, 0}, .inputFxFingerprint = 7, .monitoring = true});
	router.subscribe(
	    {.slot = SlotId::TWO, .source = {SourceType::LEFT, 0}, .inputFxFingerprint = 7, .monitoring = true});
	router.subscribe(
	    {.slot = SlotId::THREE, .source = {SourceType::LEFT, 0}, .inputFxFingerprint = 8, .monitoring = true});
	CHECK_EQUAL(1, router.sourceRenderCount());
	CHECK_EQUAL(2, router.monitorReturnCount());
}

TEST(LoopStationFanOut, InternalSourcesHaveNoDuplicateMonitorAndSelfRecordingIsRejected) {
	FanOutRouter router;
	router.bindSlotOutput(SlotId::ONE, 42);
	CHECK_FALSE(router.subscribe({.slot = SlotId::ONE, .source = {SourceType::SONG_TRACK, 42}, .monitoring = true}));
	CHECK_TRUE(router.subscribe({.slot = SlotId::TWO, .source = {SourceType::MIX_PRE_FX, 0}, .monitoring = true}));
	CHECK_EQUAL(1, router.sourceRenderCount());
	CHECK_EQUAL(0, router.monitorReturnCount());
	CHECK_FALSE(router.includeOutputInLoopStationMix(42));
	CHECK_TRUE(router.includeOutputInLoopStationMix(99));
	CHECK_FALSE(
	    router.subscribe({.slot = SlotId::THREE, .source = {SourceType::SONG_TRACK, 99}, .sourceIsAudio = false}));
}

TEST(LoopStationFanOut, BudgetGuaranteesEightStereoRecorders) {
	RecorderBudget budget;
	for (size_t i = 0; i < kTrackCount; ++i) {
		CHECK_TRUE(budget.reserve(static_cast<SlotId>(i), {.channels = 2}));
	}
	CHECK_EQUAL(RecorderBudget::kCapacity, budget.used());
	CHECK_TRUE(budget.reserve(SlotId::ONE, {.channels = 2}));
	budget.release(SlotId::ONE);
	CHECK_TRUE(budget.reserve(SlotId::ONE, {}));
}

TEST(LoopStationFanOut, RecorderCostMatchesActualAudioClipFormat) {
	CHECK_EQUAL(1, recorderChannelCount(SourceType::LEFT, false));
	CHECK_EQUAL(1, recorderChannelCount(SourceType::BALANCED, false));
	CHECK_EQUAL(2, recorderChannelCount(SourceType::STEREO, false));
	CHECK_EQUAL(2, recorderChannelCount(SourceType::MIX_PRE_FX, false));
	CHECK_EQUAL(2, recorderChannelCount(SourceType::SONG_TRACK, false));
	CHECK_EQUAL(2, recorderChannelCount(SourceType::LEFT, true));
}

TEST(LoopStationFanOut, ReplacingSubscriptionAndClearingResetRouterState) {
	FanOutRouter router;
	router.bindSlotOutput(SlotId::ONE, 42);
	CHECK_TRUE(router.subscribe({.slot = SlotId::TWO, .source = {SourceType::LEFT, 0}, .monitoring = true}));
	CHECK_TRUE(router.subscribe({.slot = SlotId::TWO, .source = {SourceType::RIGHT, 0}, .monitoring = true}));
	CHECK_EQUAL(1, router.sourceRenderCount());
	CHECK_EQUAL(1, router.monitorReturnCount());
	router.clear();
	CHECK_EQUAL(0, router.sourceRenderCount());
	CHECK_EQUAL(0, router.monitorReturnCount());
	CHECK_TRUE(router.includeOutputInLoopStationMix(42));
}

TEST(LoopStationFanOut, ReplacingAndReleasingBudgetReservationIsBalanced) {
	RecorderBudget budget;
	CHECK_TRUE(budget.reserve(SlotId::ONE, {}));
	CHECK_EQUAL(1, budget.used());
	CHECK_TRUE(budget.reserve(SlotId::ONE, {.channels = 2}));
	CHECK_EQUAL(2, budget.used());
	budget.release(SlotId::ONE);
	CHECK_EQUAL(0, budget.used());
	budget.release(SlotId::ONE);
	CHECK_EQUAL(0, budget.used());
}

TEST_GROUP(LoopStationLayers){};

TEST(LoopStationLayers, AddIsAlwaysFullFeedbackAndSaturates) {
	CHECK_EQUAL(150, saturatingAdd(100, 50));
	CHECK_EQUAL(INT32_MAX, saturatingAdd(INT32_MAX, 100));
	CHECK_EQUAL(INT32_MIN, saturatingAdd(INT32_MIN, -100));
	CHECK_EQUAL(100, saturatingAdd(100, 0));
}

TEST(LoopStationLayers, DryCaptureRestoresBackingAndInputAtEqualUnityGain) {
	constexpr int32_t oneEighth = 0x02000000;
	CHECK_EQUAL(0x10000000, restoreCaptureGain(oneEighth));
	CHECK_EQUAL(0x20000000, restoreCaptureGain(saturatingAdd(oneEighth, oneEighth)));
	CHECK_EQUAL(INT32_MAX, restoreCaptureGain(INT32_MAX / 4));
	CHECK_EQUAL(INT32_MIN, restoreCaptureGain(INT32_MIN / 4));
	CHECK_EQUAL(kCaptureGainShift, kPhysicalInputPreMixGainShift + 1);
}

TEST(LoopStationLayers, PhysicalOverdubCaptureDoesNotDependOnAudibleMonitoring) {
	TrackSettings settings;
	settings.source = SourceType::LEFT;
	settings.monitoring = false;
	TrackRuntime runtime;
	runtime.state = TrackState::OVERDUBBING;
	CHECK_TRUE(shouldCapturePhysicalOverdubInput(settings, runtime));

	settings.monitoring = true;
	CHECK_TRUE(shouldCapturePhysicalOverdubInput(settings, runtime));

	runtime.state = TrackState::PLAYING;
	runtime.finishingOverdub = true;
	CHECK_FALSE(shouldCapturePhysicalOverdubInput(settings, runtime));

	runtime.finishingOverdub = false;
	settings.source = SourceType::MIX_PRE_FX;
	CHECK_FALSE(shouldCapturePhysicalOverdubInput(settings, runtime));
}

TEST(LoopStationState, ActiveFailureCannotReenterAndRetryNeedsNoRestart) {
	SongModel model;
	Delegate delegate;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true);
	engine.centrePressed(SlotId::ONE, at(10), 1000);
	delegate.duringAbort = [&] {
		CHECK(engine.track(SlotId::ONE).finishingCapture);
		engine.notifyFailure(SlotId::ONE, Error::ALLOCATION);
		engine.centrePressed(SlotId::ONE, at(11), 2000);
		engine.stopAll(at(11));
	};
	engine.notifyFailure(SlotId::ONE, Error::SD_CARD);
	CHECK_EQUAL(1, delegate.aborts[0]);
	CHECK(engine.track(SlotId::ONE).error == Error::SD_CARD);
	CHECK_FALSE(engine.track(SlotId::ONE).finishingCapture);
	delegate.duringAbort = {};
	engine.centrePressed(SlotId::ONE, at(12), 3000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
	engine.centrePressed(SlotId::ONE, at(13), 4000);
	CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
	CHECK_EQUAL(2, delegate.begins[0]);
}

TEST(LoopStationState, StartupRejectsSameSlotGesturesButOtherSlotsRemainUsable) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	Engine engine(model, &delegate);
	bool nested = false;
	auto reenter = [&] {
		if (nested)
			return;
		nested = true;
		CHECK(engine.track(SlotId::ONE).finishingCapture);
		engine.centrePressed(SlotId::ONE, at(0), 1000);
		engine.clearHeld(SlotId::ONE);
		engine.centrePressed(SlotId::TWO, at(0), 1000);
		nested = false;
	};
	delegate.duringPreflight = reenter;
	delegate.duringBegin = reenter;
	engine.centrePressed(SlotId::ONE, at(0), 0);
	CHECK_EQUAL(1, delegate.preflights[0]);
	CHECK_EQUAL(1, delegate.begins[0]);
	CHECK_EQUAL(0, delegate.clears[0]);
	CHECK_EQUAL(1, delegate.begins[1]);
	CHECK_FALSE(engine.track(SlotId::ONE).finishingCapture);
}

TEST(LoopStationState, ExitDuringStartupWaitsAndDrainsNewRecorder) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	delegate.duringBegin = [&] { CHECK_FALSE(engine.requestExit(at(1))); };
	engine.centrePressed(SlotId::ONE, at(0), 0);
	CHECK_EQUAL(1, delegate.begins[0]);
	CHECK_EQUAL(1, delegate.finishes[0]);
	CHECK_FALSE(engine.exitReady());
	engine.notifyCommitComplete(SlotId::ONE, true);
	CHECK_TRUE(engine.exitReady());
}

TEST(LoopStationState, TransportStopDuringStartupIsNotLost) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	Engine engine(model, &delegate);
	delegate.duringBegin = [&] { engine.transportStopped(at(1)); };
	engine.centrePressed(SlotId::ONE, at(0), 0);
	CHECK_EQUAL(1, delegate.begins[0]);
	CHECK_EQUAL(1, delegate.finishes[0]);
	CHECK(engine.track(SlotId::ONE).state == TrackState::STOPPED);
}

TEST(LoopStationState, ExitWaitsForPublicationEvenWhenTrackIsPlaying) {
	SongModel model;
	Engine engine(model);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true, true, 1, 0);
	engine.track(SlotId::ONE).finishingCapture = true;
	CHECK_FALSE(engine.requestExit(at(1)));
	engine.track(SlotId::ONE).finishingCapture = false;
	CHECK_TRUE(engine.exitReady());
}

TEST(LoopStationState, OneHundredOverdubsPublishAfterDelayedCompletion) {
	SongModel model;
	model.global.startAction.mode = ActionMode::IMMEDIATE;
	Delegate delegate;
	delegate.asynchronous = true;
	Engine engine(model, &delegate);
	engine.restoreBoundPhrase(SlotId::ONE, 96, true, true, 1, 0);
	for (uint32_t i = 0; i < 100; ++i) {
		uint32_t tick = (i + 1) * 96;
		engine.centrePressed(SlotId::ONE, at(tick), tick * 1000);
		CHECK(engine.track(SlotId::ONE).state == TrackState::OVERDUBBING);
		finishDub(engine, tick + 24);
		CHECK(engine.track(SlotId::ONE).committingLength != 0);
		for (int n = 0; n < 8; ++n)
			engine.serviceAudioCompletion(at(tick + 25 + n));
		engine.notifyCommitComplete(SlotId::ONE, true);
		CHECK(engine.track(SlotId::ONE).state == TrackState::PLAYING);
		CHECK_EQUAL(i + 2, engine.track(SlotId::ONE).historySize);
	}
	CHECK_EQUAL(100, delegate.begins[0]);
	CHECK_EQUAL(100, delegate.finishes[0]);
}
