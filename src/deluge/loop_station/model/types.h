/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace deluge::loop_station::core {

constexpr size_t kSlotCount = 8;
constexpr uint16_t kManualDuration = 0;
constexpr int64_t kMaxPhraseTicks = 1610612736;
constexpr int32_t kTransposeUnity = 1 << 24;

enum class SlotId : uint8_t { ONE, TWO, THREE, FOUR, FIVE, SIX, SEVEN, EIGHT };
enum class TrackState : uint8_t { EMPTY, RECORDING, OVERDUBBING, PLAYING, STOPPED, COMMITTING, ERROR };
enum class CapturePhase : uint8_t { NONE, INITIAL_TAKE, ACTIVE_OVERDUB, FINISHING_OVERDUB, FINALIZING };
enum class RecordActionMode : uint8_t { RECORD_PLAY_OVERDUB, RECORD_OVERDUB_PLAY };
enum class RecordQuantizeMode : uint8_t { OFF, SMART, MEASURE };
enum class StartFromStoppedMode : uint8_t { RESUME_QUANTIZED, FROM_START };
enum class PhysicalSource : uint8_t { LEFT, RIGHT, STEREO, BALANCED };
enum class PlaybackMode : uint8_t { LOOP, ONE_SHOT };
enum class StartMode : uint8_t { IMMEDIATE, FADE };
enum class StopMode : uint8_t { LOOP_END, IMMEDIATE, FADE };
enum class ProgressColour : uint8_t { YELLOW, CYAN, BLUE, PURPLE, MAGENTA, WHITE };
enum class PendingAction : uint8_t { NONE, START_CAPTURE, FINISH_CAPTURE, STOP, RESUME, FINISH_FADE };
enum class Error : uint8_t {
	NONE,
	RESOURCE_LIMIT,
	ALLOCATION,
	SD_CARD,
	MAX_FILE_SIZE,
	SOURCE_INVALID,
	PITCH_SHIFTER,
	BACKING_MISSING
};

template <typename Enum, Enum Last>
constexpr Enum sanitizeEnum(Enum value, Enum fallback) {
	auto raw = static_cast<uint32_t>(value);
	return raw <= static_cast<uint32_t>(Last) ? value : fallback;
}

struct GlobalSettings {
	RecordActionMode recordAction = RecordActionMode::RECORD_PLAY_OVERDUB;
	RecordQuantizeMode recordQuantize = RecordQuantizeMode::SMART;
	StartFromStoppedMode startFromStopped = StartFromStoppedMode::RESUME_QUANTIZED;

	void sanitize();
};

struct TrackSettings {
	PhysicalSource source = PhysicalSource::LEFT;
	bool monitoring = true;
	uint16_t durationBars = kManualDuration;
	PlaybackMode playbackMode = PlaybackMode::LOOP;
	bool loopSync = true;
	StartMode startMode = StartMode::IMMEDIATE;
	StopMode stopMode = StopMode::LOOP_END;
	uint16_t fadeBars = 1;
	int8_t semitones = 0;
	int8_t cents = 0;
	ProgressColour progressColour = ProgressColour::CYAN;

	void sanitize(SlotId slot);
};

struct Revision {
	uint32_t id = 0;
	const char* filePath = nullptr;
	uint64_t startFrame = 0;
	uint64_t endFrame = 0;
	int64_t phraseLengthTicks = 0;
	int64_t sampleZeroPhaseTicks = 0;
	uint8_t channels = 1;
};

// Firmware persistence owns the revision storage; the pure model owns cursor
// semantics and deliberately never truncates redo before a successful commit.
struct HistoryState {
	int32_t cursor = -1;
	uint32_t size = 0;
	uint32_t pendingParent = std::numeric_limits<uint32_t>::max();

	void sanitize();
	[[nodiscard]] bool canUndo() const { return cursor >= 0; }
	[[nodiscard]] bool canRedo() const { return cursor + 1 < static_cast<int32_t>(size); }
	[[nodiscard]] bool beginCommit();
	void acceptCommit();
	void rejectCommit();
	[[nodiscard]] bool navigate(bool redo);
	void clear();
};

struct TrackModel {
	TrackSettings settings{};
	HistoryState history{};
	uint8_t clipBinding = 0; // persisted 1..8, zero means unbound
};

struct SongModel {
	GlobalSettings global{};
	std::array<TrackModel, kSlotCount> tracks{};

	SongModel();
	void sanitize();
};

[[nodiscard]] constexpr size_t indexOf(SlotId slot) { return static_cast<size_t>(slot); }
[[nodiscard]] constexpr int64_t floorMod(int64_t value, int64_t modulus) {
	if (modulus <= 0) return 0;
	auto result = value % modulus;
	return result < 0 ? result + modulus : result;
}
[[nodiscard]] constexpr int64_t logicalPhaseAt(int64_t tick, int64_t origin, int64_t phraseLength) {
	return floorMod(tick - origin, phraseLength);
}
[[nodiscard]] constexpr uint64_t tickPhaseToFrame(int64_t logicalPhase, Revision const& revision) {
	auto length = revision.phraseLengthTicks;
	auto frames = revision.endFrame >= revision.startFrame ? revision.endFrame - revision.startFrame : 0;
	if (length <= 0 || frames == 0) return revision.startFrame;
	auto rotated = floorMod(logicalPhase - revision.sampleZeroPhaseTicks, length);
	auto tickLength = static_cast<uint64_t>(length);
	auto phase = static_cast<uint64_t>(rotated);
	// Divide first so this remains exact without relying on __int128, which the
	// Deluge ARM toolchain does not provide. Both remainder factors are bounded
	// by kMaxPhraseTicks, so their product fits uint64_t.
	auto frameOffset = (frames / tickLength) * phase + ((frames % tickLength) * phase) / tickLength;
	return revision.startFrame + frameOffset;
}

} // namespace deluge::loop_station::core
