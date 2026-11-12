#include "timing.h"
#include <algorithm>
#include <limits>
namespace loop_station {
Pulse ClockMap::pulseAt(SampleClock sample) const {
	const bool forward = sample >= anchorSample_;
	const auto delta = forward ? sample - anchorSample_ : anchorSample_ - sample;
	const auto remainder = (delta % Pulse::scale) * bpmQ16_;
	const int64_t whole = static_cast<int64_t>((delta / Pulse::scale) * bpmQ16_ + remainder / Pulse::scale);
	const auto fraction = remainder % Pulse::scale;
	auto result = anchorPulse_;
	if (forward) {
		result.ticks += whole;
		result.fraction += fraction;
		if (result.fraction >= Pulse::scale) {
			++result.ticks;
			result.fraction -= Pulse::scale;
		}
	}
	else {
		result.ticks -= whole;
		if (result.fraction < fraction) {
			--result.ticks;
			result.fraction += Pulse::scale;
		}
		result.fraction -= fraction;
	}
	return result;
}
SampleClock ClockMap::sampleAt(Pulse pulse) const {
	int64_t ticks = pulse.ticks - anchorPulse_.ticks;
	int64_t fraction = static_cast<int64_t>(pulse.fraction) - static_cast<int64_t>(anchorPulse_.fraction);
	if (fraction < 0) {
		--ticks;
		fraction += Pulse::scale;
	}
	const bool forward = ticks >= 0;
	uint64_t whole = forward ? static_cast<uint64_t>(ticks) : static_cast<uint64_t>(-(ticks + 1));
	uint64_t part = fraction;
	if (!forward) {
		if (part == 0)
			++whole;
		else
			part = Pulse::scale - part;
	}
	const uint64_t rest = (whole % bpmQ16_) * Pulse::scale + part;
	// ceil(anchor + distance) for future targets; anchor - floor(distance)
	// for earlier pulses, including pulses before a future metronome offset.
	const auto delta = (whole / bpmQ16_) * Pulse::scale + rest / bpmQ16_ + (forward && rest % bpmQ16_ != 0);
	if (!forward)
		return delta > anchorSample_ ? 0 : anchorSample_ - delta;
	return delta > std::numeric_limits<SampleClock>::max() - anchorSample_ ? std::numeric_limits<SampleClock>::max()
	                                                                       : anchorSample_ + delta;
}
void ClockMap::setTempoQ16(SampleClock now, uint32_t bpmQ16) {
	if (bpmQ16 == 0 || bpmQ16 > 1000U * 65536)
		return;
	anchorPulse_ = pulseAt(now);
	anchorSample_ = now;
	bpmQ16_ = bpmQ16;
}
void ClockMap::setTransport(bool running, SampleClock now) {
	if (running && !running_) {
		// Keep queued absolute musical targets continuous across transport start.
		anchorPulse_ = pulseAt(now);
		anchorSample_ = now;
		metronomeOrigin_ = anchorPulse_;
	}
	running_ = running;
}
void ClockMap::setMetronomePositionQ16(SampleClock now, int64_t tickQ16) {
	metronomeOrigin_ = pulseAt(now);
	auto whole = tickQ16 / 65536;
	auto fraction = tickQ16 % 65536;
	if (fraction < 0) {
		--whole;
		fraction += 65536;
	}
	metronomeOrigin_.ticks -= whole;
	const auto part = uint64_t(fraction) * (Pulse::scale / 65536);
	if (metronomeOrigin_.fraction < part) {
		--metronomeOrigin_.ticks;
		metronomeOrigin_.fraction += Pulse::scale;
	}
	metronomeOrigin_.fraction -= part;
}
FrameIndex ClockMap::samplesForBars(uint16_t bars) const {
	const uint64_t numerator = uint64_t(bars) * 96 * Pulse::scale;
	const auto frames = numerator / bpmQ16_ + (numerator % bpmQ16_ != 0);
	return static_cast<FrameIndex>(std::min<uint64_t>(frames, std::numeric_limits<FrameIndex>::max()));
}
int64_t ClockMap::quarterQ16(SampleClock now) const {
	const auto pulse = pulseAt(now);
	auto quarters = pulse.ticks / 24;
	auto ticks = pulse.ticks % 24;
	if (ticks < 0) {
		--quarters;
		ticks += 24;
	}
	return quarters * 65536 + (ticks * 65536 + static_cast<int64_t>(pulse.fraction * 65536 / Pulse::scale)) / 24;
}
Deadline nextDeadline(ActionSettings settings, SampleClock now, const ClockMap& map) {
	if (settings.mode == ActionMode::Immediate)
		return {true, now, {}};
	// Spacing in 1/24-quarter ticks, then number of strictly future pulses.
	constexpr unsigned spacing[] = {24, 24, 24, 12, 16, 16, 16, 8, 6, 3, 4, 2};
	constexpr unsigned count[] = {4, 2, 1, 1, 4, 2, 1, 1, 1, 1, 1, 1};
	auto id = static_cast<unsigned>(settings.interval);
	if (id >= 12)
		id = 8;
	const auto pulse = map.pulseAt(now);
	auto target = map.metronomeOrigin();
	auto ticks = pulse.ticks - target.ticks;
	if (pulse.fraction < target.fraction)
		--ticks;
	auto index = ticks / static_cast<int64_t>(spacing[id]);
	if (ticks < 0 && ticks % spacing[id] != 0)
		--index;
	target.ticks += (index + count[id]) * spacing[id];
	return {false, 0, target};
}
Deadline durationDeadline(uint16_t bars, SampleClock now, const ClockMap& map) {
	auto pulse = map.pulseAt(now);
	pulse.ticks += uint64_t(bars) * 96;
	return {false, 0, pulse};
}
} // namespace loop_station
