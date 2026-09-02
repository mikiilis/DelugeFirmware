#include "loop_station/settings/adapters.h"

#include <algorithm>

namespace deluge::loop_station::core::settings {

int32_t read(GlobalSettings const& s, GlobalParameter p) {
	switch (p) {
	case GlobalParameter::RECORD_ACTION: return static_cast<int32_t>(s.recordAction);
	case GlobalParameter::RECORD_QUANTIZE: return static_cast<int32_t>(s.recordQuantize);
	case GlobalParameter::START_FROM_STOPPED: return static_cast<int32_t>(s.startFromStopped);
	}
	return 0;
}

void write(GlobalSettings& s, GlobalParameter p, int32_t value) {
	switch (p) {
	case GlobalParameter::RECORD_ACTION: s.recordAction = static_cast<RecordActionMode>(value); break;
	case GlobalParameter::RECORD_QUANTIZE: s.recordQuantize = static_cast<RecordQuantizeMode>(value); break;
	case GlobalParameter::START_FROM_STOPPED: s.startFromStopped = static_cast<StartFromStoppedMode>(value); break;
	}
	s.sanitize();
}

int32_t read(TrackSettings const& s, TrackParameter p) {
	switch (p) {
	case TrackParameter::SOURCE: return static_cast<int32_t>(s.source);
	case TrackParameter::MONITORING: return s.monitoring;
	case TrackParameter::DURATION: return s.durationBars;
	case TrackParameter::PLAYBACK: return static_cast<int32_t>(s.playbackMode);
	case TrackParameter::LOOP_SYNC: return s.loopSync;
	case TrackParameter::START_MODE: return static_cast<int32_t>(s.startMode);
	case TrackParameter::STOP_MODE: return static_cast<int32_t>(s.stopMode);
	case TrackParameter::FADE_BARS: return s.fadeBars;
	case TrackParameter::SEMITONES: return s.semitones;
	case TrackParameter::CENTS: return s.cents;
	case TrackParameter::PROGRESS_COLOUR: return static_cast<int32_t>(s.progressColour);
	}
	return 0;
}

void write(TrackSettings& s, SlotId slot, TrackParameter p, int32_t value) {
	switch (p) {
	case TrackParameter::SOURCE: s.source = static_cast<PhysicalSource>(value); break;
	case TrackParameter::MONITORING: s.monitoring = value != 0; break;
	case TrackParameter::DURATION: s.durationBars = std::clamp<int32_t>(value, 0, UINT16_MAX); break;
	case TrackParameter::PLAYBACK: s.playbackMode = static_cast<PlaybackMode>(value); break;
	case TrackParameter::LOOP_SYNC: s.loopSync = value != 0; break;
	case TrackParameter::START_MODE: s.startMode = static_cast<StartMode>(value); break;
	case TrackParameter::STOP_MODE: s.stopMode = static_cast<StopMode>(value); break;
	case TrackParameter::FADE_BARS: s.fadeBars = std::clamp<int32_t>(value, 1, UINT16_MAX); break;
	case TrackParameter::SEMITONES: s.semitones = std::clamp<int32_t>(value, -24, 24); break;
	case TrackParameter::CENTS: s.cents = std::clamp<int32_t>(value, -50, 50); break;
	case TrackParameter::PROGRESS_COLOUR: s.progressColour = static_cast<ProgressColour>(value); break;
	}
	s.sanitize(slot);
}

} // namespace deluge::loop_station::core::settings
