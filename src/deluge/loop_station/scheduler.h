/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "loop_station/loop_station.h"
#include <algorithm>
#include <limits>

namespace deluge::loop_station {

// Bounded RAM-only diagnostics: no printing, allocation, or SD writes on the
// audio thread. Inspect in a debugger when reproducing timing on hardware.
struct SchedulerTraceEntry {
	uint32_t schedulerTick;
	uint32_t observedMusicalTick;
	uint32_t barLength;
	uint32_t targetTick;
	uint8_t slot;
	PendingAction action;
	PlaybackTiming startTiming;
	PlaybackTiming stopTiming;
};
inline volatile SchedulerTraceEntry schedulerTrace[32]{};
inline volatile uint32_t schedulerTraceSequence = 0;

// Called only by Session's musical scheduler, never by graphics. Both event
// execution and returned delay use the same scheduler origin. Gesture/display
// clocks may be ahead of this tick when the scheduler is catching up.
inline int32_t serviceSchedulerTick(Engine& engine, Transport transport, uint32_t schedulerTick,
                                    int32_t existingDelay) {
	for (size_t i = 0; i < kTrackCount; ++i) {
		auto const& event = engine.track(static_cast<SlotId>(i)).event;
		if (!event.pending())
			continue;
		uint32_t sequence = schedulerTraceSequence;
		auto& trace = schedulerTrace[sequence % 32];
		trace.schedulerTick = schedulerTick;
		trace.observedMusicalTick = transport.tick;
		trace.barLength = transport.barLength;
		trace.targetTick = event.tick;
		trace.slot = i;
		trace.action = event.action;
		trace.startTiming = engine.model().global.startTiming;
		trace.stopTiming = engine.model().global.stopTiming;
		schedulerTraceSequence = sequence + 1;
	}
	transport.tick = schedulerTick;
	transport.afterTickBoundary = false;
	engine.processTick(transport);
	uint32_t delay = engine.ticksUntilNextEvent(schedulerTick);
	if (delay == std::numeric_limits<uint32_t>::max()) {
		return existingDelay;
	}
	return std::min(existingDelay, static_cast<int32_t>(std::min<uint32_t>(std::max<uint32_t>(1, delay),
	                                                                       std::numeric_limits<int32_t>::max())));
}

} // namespace deluge::loop_station
