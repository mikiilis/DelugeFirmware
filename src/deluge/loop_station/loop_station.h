/*
 * Copyright © 2026 Deluge Community Firmware contributors
 *
 * This file is part of the Deluge Firmware and is licensed under GPL-3.0-or-later.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace deluge::loop_station {

constexpr size_t kTrackCount = 8;
constexpr size_t kCellBorderPadCount = 12;
constexpr uint16_t kManualDuration = 0;
constexpr uint32_t kInvalidSlotBinding = 0;
constexpr uint32_t kMaxLoopSequenceLength = 1610612736;

enum class SlotId : uint8_t { ONE, TWO, THREE, FOUR, FIVE, SIX, SEVEN, EIGHT };
enum class RecordAction : uint8_t { RECORD_OVERDUB_PLAY, RECORD_PLAY_OVERDUB };
enum class QuantizeMode : uint8_t { OFF, SMART, MEASURE };
enum class TrackState : uint8_t { EMPTY, RECORDING, OVERDUBBING, PLAYING, STOPPED, COMMITTING, ERROR };
enum class StartMode : uint8_t { IMMEDIATE, FADE };
enum class StopMode : uint8_t { LOOP_END, IMMEDIATE, FADE };
enum class PlaybackMode : uint8_t { LOOP, ONE_SHOT };
enum class SourceType : uint8_t { LEFT, RIGHT, STEREO, BALANCED, MIX_PRE_FX, MIX_POST_FX, SONG_TRACK };
enum class PadColour : uint8_t { RED, ORANGE, YELLOW, GREEN, CYAN, BLUE, PURPLE, MAGENTA, WHITE };
enum class PendingAction : uint8_t {
	NONE,
	START_RECORDING,
	FINISH_RECORDING,
	START_OVERDUB,
	FINISH_OVERDUB,
	STOP,
	RETRIGGER
};
enum class Error : uint8_t { NONE, RESOURCE_LIMIT, ALLOCATION, SD_CARD, MAX_FILE_SIZE, SOURCE_REMOVED };

// Performance actions are dispatched by the first pad-down, not the eventual
// release of the coalesced 2x2 gesture. STOPPED is the sole exception because
// its release distinguishes a normal Play tap from the two-second Clear hold.
[[nodiscard]] constexpr bool centreGestureActsOnPress(TrackState state, bool shiftHeld) {
	return !shiftHeld && state != TrackState::STOPPED;
}

[[nodiscard]] constexpr bool isPhysicalSource(SourceType source) {
	return source == SourceType::LEFT || source == SourceType::RIGHT || source == SourceType::STEREO
	       || source == SourceType::BALANCED;
}

[[nodiscard]] constexpr int32_t saturatingAdd(int32_t backing, int32_t input) {
	return static_cast<int32_t>(std::clamp<int64_t>(static_cast<int64_t>(backing) + input,
	                                                std::numeric_limits<int32_t>::min(),
	                                                std::numeric_limits<int32_t>::max()));
}

// Stable setting identifiers shared by the firmware menu layer and host tests.
enum class GlobalParameter : uint8_t { RECORD_ACTION, QUANTIZE, DIM_BORDER_COLOUR };
enum class TrackParameter : uint8_t {
	SOURCE,
	SOURCE_TRACK,
	MONITORING,
	DURATION_BARS,
	PROGRESS_COLOUR,
	PLAYBACK,
	LOOP_SYNC,
	START_MODE,
	STOP_MODE,
	FADE_TIME_BARS
};

template <typename Enum, Enum Last>
constexpr Enum clampEnum(uint32_t raw, Enum fallback) {
	return raw <= static_cast<uint32_t>(Last) ? static_cast<Enum>(raw) : fallback;
}

struct GlobalSettings {
	RecordAction recordAction = RecordAction::RECORD_PLAY_OVERDUB;
	QuantizeMode quantize = QuantizeMode::SMART;
	PadColour dimBorderColour = PadColour::BLUE;
};

constexpr std::array<PadColour, 6> kProgressColours{PadColour::YELLOW, PadColour::CYAN,    PadColour::BLUE,
                                                    PadColour::PURPLE, PadColour::MAGENTA, PadColour::WHITE};
[[nodiscard]] constexpr bool isReservedStateColour(PadColour colour) {
	return colour == PadColour::RED || colour == PadColour::ORANGE || colour == PadColour::GREEN;
}
[[nodiscard]] constexpr PadColour defaultProgressColour(SlotId slot) {
	return kProgressColours[static_cast<size_t>(slot) % kProgressColours.size()];
}

struct TrackSettings {
	SourceType source = SourceType::LEFT;
	uint32_t sourceTrackId = 0;
	bool monitoring = true;
	uint16_t durationBars = kManualDuration;
	PadColour progressColour = PadColour::CYAN;
	PlaybackMode playback = PlaybackMode::LOOP;
	bool loopSync = true;
	StartMode startMode = StartMode::IMMEDIATE;
	StopMode stopMode = StopMode::LOOP_END;
	uint16_t fadeTimeBars = 1;
};

struct TrackBinding {
	// Stable ID persisted on the backing Audio Clip. Zero means unbound.
	uint32_t clipSlotId = kInvalidSlotBinding;
	TrackSettings settings{};
};

struct SongModel {
	GlobalSettings global{};
	std::array<TrackBinding, kTrackCount> tracks{};

	SongModel();
	void sanitize();
};

[[nodiscard]] int32_t readGlobalParameter(SongModel const& model, GlobalParameter parameter);
void writeGlobalParameter(SongModel& model, GlobalParameter parameter, int32_t value);
[[nodiscard]] int32_t readTrackParameter(SongModel const& model, SlotId slot, TrackParameter parameter);
void writeTrackParameter(SongModel& model, SlotId slot, TrackParameter parameter, int32_t value);

struct Transport {
	uint32_t tick = 0;
	uint32_t barLength = 1;
	uint32_t eighthNoteLength = 1;
	bool playing = false;
	bool arrangement = false;
};

struct ScheduledEvent {
	PendingAction action = PendingAction::NONE;
	uint32_t tick = 0;
	uint32_t requestedTick = 0;

	[[nodiscard]] bool pending() const { return action != PendingAction::NONE; }
};

struct TrackRuntime {
	TrackState state = TrackState::EMPTY;
	TrackState stateBeforeCommit = TrackState::EMPTY;
	ScheduledEvent event{};
	uint32_t captureStartTick = 0;
	uint32_t phraseLength = 0;
	uint32_t playbackStartTick = 0;
	// Overdubs are bounded by the already-recorded phrase. This is a runtime
	// deadline rather than a ScheduledEvent so the UI does not render the whole
	// overdub as a pending (blinking) operation.
	uint32_t overdubEndTick = 0;
	uint32_t committingLength = 0;
	uint32_t currentRevision = 0;
	bool redoAvailable = false;
	bool undoAvailable = false;
	uint32_t historySize = 0;
	int32_t historyCursor = -1;
	bool provisionalOverdub = false;
	// The UI has returned to Play, but the recorder is completing the untouched
	// remainder of a partial overdub pass before swapping the backing WAV.
	bool finishingOverdub = false;
	bool pendingClear = false;
	bool committingOverdub = false;
	Error error = Error::NONE;
	uint32_t lastTapTimeMs = std::numeric_limits<uint32_t>::max();
};

// Audible monitoring is deliberately not part of this decision. A disabled
// monitor return must never disable the signal being written into an overdub.
[[nodiscard]] constexpr bool shouldCapturePhysicalOverdubInput(TrackSettings const& settings,
                                                               TrackRuntime const& runtime) {
	return isPhysicalSource(settings.source) && !runtime.finishingOverdub;
}

// The first take owns the logical phase-zero for the lifetime of the phrase.
// A cumulative WAV recorded by starting an overdub mid-loop has its physical
// sample-zero at a different logical phase, so keep those concepts separate.
[[nodiscard]] constexpr uint32_t loopPhaseAt(uint32_t tick, uint32_t playbackStartTick, uint32_t phraseLength) {
	if (phraseLength == 0 || tick < playbackStartTick) {
		return 0;
	}
	return (tick - playbackStartTick) % phraseLength;
}

[[nodiscard]] constexpr uint32_t samplePhaseForLoopPhase(uint32_t loopPhase, uint32_t sampleZeroLoopPhase,
                                                         uint32_t phraseLength) {
	if (phraseLength == 0) {
		return 0;
	}
	loopPhase %= phraseLength;
	sampleZeroLoopPhase %= phraseLength;
	return loopPhase >= sampleZeroLoopPhase ? loopPhase - sampleZeroLoopPhase
	                                        : phraseLength - (sampleZeroLoopPhase - loopPhase);
}

struct CaptureFinish {
	bool accepted = true;
	bool asynchronous = false;
	// Zero keeps the engine-requested length. A recorder may return an adjusted
	// whole-tick length after reconciling the captured samples with the exact
	// gesture position, preventing an inaccurate scheduler length from changing
	// playback speed.
	uint32_t committedLength = 0;
};

class EngineDelegate {
public:
	virtual ~EngineDelegate() = default;
	virtual bool preflight(SlotId, bool) { return true; }
	virtual bool beginCapture(SlotId, bool, uint32_t) { return true; }
	virtual CaptureFinish finishCapture(SlotId, bool, uint32_t, bool) { return {}; }
	virtual void abortCapture(SlotId, bool) {}
	virtual void phraseCleared(SlotId) {}
	// Move exactly one revision. targetHasPhrase reports whether the resulting
	// cursor points at a WAV or at the undoable empty state.
	virtual bool navigateHistory(SlotId, bool redo, bool& targetHasPhrase) { return true; }
	virtual void stateChanged(SlotId, TrackState) {}
};

class Engine {
public:
	explicit Engine(SongModel& model, EngineDelegate* delegate = nullptr);

	void resetRuntime();
	void restoreBoundPhrase(SlotId slot, uint32_t phraseLength, bool playing, bool hasPhrase = true,
	                        uint32_t historySize = 1, int32_t historyCursor = 0);
	void setDelegate(EngineDelegate* delegate) { delegate_ = delegate; }
	[[nodiscard]] SongModel& model() { return model_; }
	[[nodiscard]] TrackRuntime const& track(SlotId slot) const;
	[[nodiscard]] TrackRuntime& track(SlotId slot);

	// Called once for the coalesced 2x2 centre gesture, on release.
	void centrePressed(SlotId slot, Transport const& transport, uint32_t gestureTimeMs);
	void clearHeld(SlotId slot);
	bool undo(SlotId slot);
	bool redo(SlotId slot);
	void processTick(Transport const& transport);
	bool requestExit(Transport const& transport);
	[[nodiscard]] bool exitReady() const;
	[[nodiscard]] bool anyCapturing() const;
	[[nodiscard]] uint32_t ticksUntilNextEvent(uint32_t currentTick) const;
	void notifyCommitComplete(SlotId slot, bool success, Error error = Error::NONE);
	void notifyFailure(SlotId slot, Error error);

	static uint32_t quantizedStart(QuantizeMode mode, uint32_t tick, uint32_t barLength);
	static uint32_t smartStop(uint32_t tick, uint32_t barLength, uint32_t eighthNoteLength);

private:
	void schedule(SlotId slot, PendingAction action, uint32_t atTick, uint32_t requestedTick);
	void execute(SlotId slot, PendingAction action, Transport const& transport, uint32_t requestedTick);
	void beginCapture(SlotId slot, bool overdub, uint32_t tick, Transport const& transport);
	void finishCapture(SlotId slot, bool overdub, uint32_t tick, uint32_t requestedTick);
	void stop(SlotId slot, uint32_t tick);
	void completeCapture(SlotId slot, bool overdub, uint32_t length);
	void setOverdubEnd(TrackRuntime& runtime, uint32_t tick);
	void transition(SlotId slot, TrackState state);
	void fail(SlotId slot, Error error);
	bool navigateHistory(SlotId slot, bool redo);
	void refreshHistoryAvailability(SlotId slot);
	bool isDoubleTap(TrackRuntime& runtime, uint32_t gestureTimeMs);

	SongModel& model_;
	EngineDelegate* delegate_ = nullptr;
	std::array<TrackRuntime, kTrackCount> runtime_{};
	bool exiting_ = false;
	uint32_t nextRevision_ = 1;
};

struct PadCoordinate {
	uint8_t x;
	uint8_t y;
	constexpr bool operator==(PadCoordinate const&) const = default;
};

class Geometry {
public:
	struct ProgressFill {
		uint8_t fullPads = 0;
		uint16_t leadingBrightness = 0;
	};

	static constexpr std::array<PadCoordinate, kCellBorderPadCount> kClockwiseBorder{
	    {{0, 3}, {1, 3}, {2, 3}, {3, 3}, {3, 2}, {3, 1}, {3, 0}, {2, 0}, {1, 0}, {0, 0}, {0, 1}, {0, 2}}};

	static constexpr PadCoordinate origin(SlotId slot) {
		auto index = static_cast<uint8_t>(slot);
		return PadCoordinate{static_cast<uint8_t>((index % 4) * 4), static_cast<uint8_t>((index / 4) * 4)};
	}
	static constexpr bool isCentre(uint8_t localX, uint8_t localY) {
		return localX >= 1 && localX <= 2 && localY >= 1 && localY <= 2;
	}
	static constexpr SlotId slotAt(uint8_t x, uint8_t y) { return static_cast<SlotId>((y / 4) * 4 + (x / 4)); }
	static uint8_t progressPadCount(uint32_t position, uint32_t phraseLength);
	// positionQ16 is a musical position in ticks with a 16-bit fractional part.
	static ProgressFill progressFill(uint64_t positionQ16, uint32_t phraseLength);
	// Continuous triangle pulse with its brightest point on each beat boundary.
	static uint16_t beatPulseBrightness(uint64_t positionQ16, uint32_t beatLength);
};

struct SourceKey {
	SourceType type = SourceType::LEFT;
	uint32_t songTrackId = 0;
	constexpr bool operator==(SourceKey const&) const = default;
};

struct TapSubscription {
	SlotId slot = SlotId::ONE;
	SourceKey source{};
	uint32_t inputFxFingerprint = 0;
	bool monitoring = false;
	bool capturing = false;
	bool sourceIsAudio = true;
};

// Builds one render group per shared source and one monitor return per unique
// physical-source/input-FX pair. Audio rendering remains owned by AudioEngine.
class FanOutRouter {
public:
	void clear();
	bool subscribe(TapSubscription subscription);
	[[nodiscard]] size_t sourceRenderCount() const;
	[[nodiscard]] size_t monitorReturnCount() const;
	[[nodiscard]] bool sourceFeedsItself(SlotId destination, SourceKey source) const;
	void bindSlotOutput(SlotId slot, uint32_t outputId);
	[[nodiscard]] bool includeOutputInLoopStationMix(uint32_t outputId) const;

private:
	std::array<TapSubscription, kTrackCount> subscriptions_{};
	std::array<bool, kTrackCount> used_{};
	std::array<uint32_t, kTrackCount> slotOutputIds_{};
};

struct RecorderCost {
	uint8_t channels = 1;
	uint8_t inputFxUnits = 0;
	bool internalSource = false;
};

class RecorderBudget {
public:
	static constexpr uint16_t kCapacity = 16; // exactly eight default mono recorders
	bool reserve(SlotId slot, RecorderCost cost);
	void release(SlotId slot);
	[[nodiscard]] uint16_t used() const { return used_; }

private:
	static uint16_t weight(RecorderCost cost);
	std::array<uint16_t, kTrackCount> reservations_{};
	uint16_t used_ = 0;
};

} // namespace deluge::loop_station
