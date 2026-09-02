#pragma once

#include "loop_station/engine/engine.h"

#include <limits>

namespace deluge::loop_station::core::playback {

// Session's swung scheduler calls this bridge at exact event ticks and uses
// the returned value to tighten swungTicksTilNextEvent.
class Bridge {
public:
	explicit Bridge(Engine& engine) : engine_(engine) {}
	void swungTick(TransportTime const& time) { engine_.process(time); }
	void reduceTicksUntilNextEvent(int64_t currentTick, int64_t& ticks) const {
		ticks = std::min(ticks, engine_.ticksUntilNextEvent(currentTick));
	}

private:
	Engine& engine_;
};

} // namespace deluge::loop_station::core::playback
