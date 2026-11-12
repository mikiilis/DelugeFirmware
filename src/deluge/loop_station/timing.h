#pragma once
#include "config.h"
namespace loop_station {
// A tick is 1/24 quarter note: all straight/triplet pulses are integral ticks.
// Fixed denominator integrates every Q16 BPM exactly, including tempo edits.
struct Pulse {
	static constexpr uint64_t scale = 110250ULL * 65536;
	int64_t ticks = 0;
	uint64_t fraction = 0;
};
class ClockMap {
public:
	explicit ClockMap(SampleClock metronomeOffset = 0) : anchorSample_(metronomeOffset) {}
	Pulse pulseAt(SampleClock sample) const;
	SampleClock sampleAt(Pulse pulse) const;
	Pulse metronomeOrigin() const { return metronomeOrigin_; }
	// BPM is supplied in Q16 by the firmware adapter; tempo edits preserve phase.
	void setTempoQ16(SampleClock now, uint32_t bpmQ16);
	void setTempo(SampleClock now, uint16_t bpm) { setTempoQ16(now, uint32_t(bpm) * 65536); }
	void setTransport(bool running, SampleClock now);
	// Actual unswung song tick plus metronome offset, normalized to 24 ticks/quarter.
	// Adjust only the pulse grid origin; phrase and queued target positions stay continuous.
	void setMetronomePositionQ16(SampleClock now, int64_t tickQ16);
	FrameIndex samplesForBars(uint16_t bars) const;
	int64_t quarterQ16(SampleClock now) const;

private:
	SampleClock anchorSample_ = 0;
	Pulse anchorPulse_{};
	Pulse metronomeOrigin_{};
	uint32_t bpmQ16_ = 120 * 65536;
	bool running_ = false;
};
struct Deadline {
	bool immediate = false;
	SampleClock sample = 0;
	Pulse pulse{};
	SampleClock sampleAt(const ClockMap& map) const { return immediate ? sample : map.sampleAt(pulse); }
};
Deadline nextDeadline(ActionSettings settings, SampleClock now, const ClockMap& map);
Deadline durationDeadline(uint16_t bars, SampleClock now, const ClockMap& map);
// Observe at least once per 32-bit wrap. The adapter supplies the device sample timer.
class SampleClockExtender {
public:
	SampleClock observe(uint32_t sample) {
		if (!initialized_) {
			initialized_ = true;
			extended_ = sample;
		}
		else
			extended_ += uint32_t(sample - last_);
		last_ = sample;
		return extended_;
	}

private:
	SampleClock extended_ = 0;
	uint32_t last_ = 0;
	bool initialized_ = false;
};
} // namespace loop_station
