#pragma once

#include "loop_station/model/types.h"

namespace deluge::loop_station::core::settings {

enum class GlobalParameter : uint8_t { RECORD_ACTION, RECORD_QUANTIZE, START_FROM_STOPPED };
enum class TrackParameter : uint8_t {
	SOURCE,
	MONITORING,
	DURATION,
	PLAYBACK,
	LOOP_SYNC,
	START_MODE,
	STOP_MODE,
	FADE_BARS,
	SEMITONES,
	CENTS,
	PROGRESS_COLOUR
};

int32_t read(GlobalSettings const& settings, GlobalParameter parameter);
void write(GlobalSettings& settings, GlobalParameter parameter, int32_t value);
int32_t read(TrackSettings const& settings, TrackParameter parameter);
void write(TrackSettings& settings, SlotId slot, TrackParameter parameter, int32_t value);

} // namespace deluge::loop_station::core::settings
