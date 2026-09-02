#include "loop_station/model/types.h"

#include <algorithm>

namespace deluge::loop_station::core {

void GlobalSettings::sanitize() {
	recordAction = sanitizeEnum<RecordActionMode, RecordActionMode::RECORD_OVERDUB_PLAY>(
	    recordAction, RecordActionMode::RECORD_PLAY_OVERDUB);
	recordQuantize = sanitizeEnum<RecordQuantizeMode, RecordQuantizeMode::MEASURE>(recordQuantize,
	                                                                               RecordQuantizeMode::SMART);
	startFromStopped = sanitizeEnum<StartFromStoppedMode, StartFromStoppedMode::FROM_START>(
	    startFromStopped, StartFromStoppedMode::RESUME_QUANTIZED);
}

void TrackSettings::sanitize(SlotId slot) {
	source = sanitizeEnum<PhysicalSource, PhysicalSource::BALANCED>(source, PhysicalSource::LEFT);
	playbackMode = sanitizeEnum<PlaybackMode, PlaybackMode::ONE_SHOT>(playbackMode, PlaybackMode::LOOP);
	startMode = sanitizeEnum<StartMode, StartMode::FADE>(startMode, StartMode::IMMEDIATE);
	stopMode = sanitizeEnum<StopMode, StopMode::FADE>(stopMode, StopMode::LOOP_END);
	progressColour = sanitizeEnum<ProgressColour, ProgressColour::WHITE>(
	    progressColour, static_cast<ProgressColour>(indexOf(slot) % 6));
	fadeBars = std::max<uint16_t>(fadeBars, 1);
	semitones = std::clamp<int8_t>(semitones, -24, 24);
	cents = std::clamp<int8_t>(cents, -50, 50);
}

void HistoryState::sanitize() {
	if (size == 0) cursor = -1;
	else cursor = std::clamp<int32_t>(cursor, -1, static_cast<int32_t>(size) - 1);
	pendingParent = std::numeric_limits<uint32_t>::max();
}

bool HistoryState::beginCommit() {
	if (pendingParent != std::numeric_limits<uint32_t>::max()) return false;
	pendingParent = static_cast<uint32_t>(cursor + 1);
	return true;
}

void HistoryState::acceptCommit() {
	if (pendingParent == std::numeric_limits<uint32_t>::max()) return;
	size = pendingParent + 1;
	cursor = static_cast<int32_t>(pendingParent);
	pendingParent = std::numeric_limits<uint32_t>::max();
}

void HistoryState::rejectCommit() { pendingParent = std::numeric_limits<uint32_t>::max(); }

bool HistoryState::navigate(bool redo) {
	if (pendingParent != std::numeric_limits<uint32_t>::max()) return false;
	if (redo ? !canRedo() : !canUndo()) return false;
	cursor += redo ? 1 : -1;
	return true;
}

void HistoryState::clear() { *this = {}; }

SongModel::SongModel() {
	for (size_t i = 0; i < kSlotCount; ++i) {
		tracks[i].settings.progressColour = static_cast<ProgressColour>(i % 6);
	}
}

void SongModel::sanitize() {
	global.sanitize();
	std::array<bool, kSlotCount + 1> claimed{};
	for (size_t i = 0; i < kSlotCount; ++i) {
		auto& track = tracks[i];
		track.settings.sanitize(static_cast<SlotId>(i));
		track.history.sanitize();
		if (track.clipBinding > kSlotCount || (track.clipBinding != 0 && claimed[track.clipBinding])) {
			track.clipBinding = 0;
		}
		if (track.clipBinding) claimed[track.clipBinding] = true;
	}
}

} // namespace deluge::loop_station::core
