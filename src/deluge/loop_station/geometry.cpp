#include "geometry.h"

namespace loop_station {

PadAddress padAddress(int32_t x, int32_t y) {
	if (x < 0 || x >= 16 || y < 0 || y >= 8)
		return {};

	const int32_t visualY = 7 - y;
	const auto localX = static_cast<uint8_t>(x % 4);
	const auto localY = static_cast<uint8_t>(visualY % 4);
	const auto slot = static_cast<SlotId>((visualY / 4) * 4 + x / 4);
	if (localX >= 1 && localX <= 2 && localY >= 1 && localY <= 2)
		return {slot, PadRegion::Centre, 0, true};

	uint8_t borderIndex = 0;
	if (localY == 0)
		borderIndex = localX;
	else if (localX == 3)
		borderIndex = static_cast<uint8_t>(3 + localY);
	else if (localY == 3)
		borderIndex = static_cast<uint8_t>(9 - localX);
	else
		borderIndex = static_cast<uint8_t>(12 - localY);
	return {slot, PadRegion::Border, borderIndex, true};
}

uint8_t filledBorderPads(FrameIndex position, FrameIndex phraseFrames) {
	if (phraseFrames == 0)
		return 0;
	const uint64_t phase = position % phraseFrames;
	return static_cast<uint8_t>(1 + (12ull * phase) / phraseFrames);
}

uint8_t clearHoldSweepPads(SampleClock heldSamples, SampleClock holdDurationSamples) {
	if (holdDurationSamples == 0)
		return 0;
	const SampleClock bounded = heldSamples < holdDurationSamples ? heldSamples : holdDurationSamples;
	return static_cast<uint8_t>((12ull * bounded) / holdDurationSamples);
}

bool visualPulseOn(SampleClock sample, SampleClock halfPeriodSamples) {
	return halfPeriodSamples != 0 && ((sample / halfPeriodSamples) & 1u) != 0;
}

} // namespace loop_station
