#include "loop_station/audio/audio.h"

#include <algorithm>
#include <cmath>

namespace deluge::loop_station::core::audio {

bool RecorderBudget::reserve(RecorderRequest request) {
	auto index = indexOf(request.slot);
	auto channels = std::clamp<uint8_t>(request.channels, 1, 2);
	auto without = static_cast<uint8_t>(used_ - reservations_[index]);
	if (without + channels > kChannelCapacity)
		return false;
	reservations_[index] = channels;
	used_ = without + channels;
	return true;
}

void RecorderBudget::release(SlotId slot) {
	auto index = indexOf(slot);
	used_ -= reservations_[index];
	reservations_[index] = 0;
}

void PhysicalInputRouter::clear() {
	used_.fill(false);
}

void PhysicalInputRouter::subscribe(RouteRequest request) {
	auto index = indexOf(request.slot);
	routes_[index] = request;
	used_[index] = true;
}

bool PhysicalInputRouter::shouldRenderSource(PhysicalSource source) const {
	for (size_t i = 0; i < kSlotCount; ++i)
		if (used_[i] && routes_[i].source == source)
			return true;
	return false;
}

bool PhysicalInputRouter::monitorAudible(SlotId slot) const {
	auto index = indexOf(slot);
	if (!used_[index] || !routes_[index].monitoring)
		return false;
	auto const& route = routes_[index];
	if (route.capturing)
		return true;
	if (!route.selected)
		return false;
	for (size_t i = 0; i < kSlotCount; ++i) {
		if (i != index && used_[i] && routes_[i].capturing && routes_[i].source == route.source)
			return false;
	}
	return true;
}

size_t PhysicalInputRouter::sourceReadCount() const {
	size_t count = 0;
	for (uint8_t source = 0; source <= static_cast<uint8_t>(PhysicalSource::BALANCED); ++source) {
		count += shouldRenderSource(static_cast<PhysicalSource>(source));
	}
	return count;
}

size_t PhysicalInputRouter::monitorReturnCount() const {
	size_t count = 0;
	for (size_t i = 0; i < kSlotCount; ++i) {
		if (!monitorAudible(static_cast<SlotId>(i)))
			continue;
		bool duplicate = false;
		for (size_t j = 0; j < i; ++j) {
			if (monitorAudible(static_cast<SlotId>(j)) && routes_[j].source == routes_[i].source
			    && routes_[j].processingSignature == routes_[i].processingSignature) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			++count;
	}
	return count;
}

DryMixResult mixDryCumulative(std::span<int32_t const> backing, uint8_t backingChannels, std::span<int32_t const> input,
                              uint8_t inputChannels, std::span<int32_t> destination, bool backingRequired) {
	if (backingRequired && backing.empty())
		return {.accepted = false};
	backingChannels = std::clamp<uint8_t>(backingChannels, 1, 2);
	inputChannels = std::clamp<uint8_t>(inputChannels, 1, 2);
	auto outputChannels = std::max(backingChannels, inputChannels);
	auto backingFrames = backing.size() / backingChannels;
	auto inputFrames = input.size() / inputChannels;
	auto frames = std::min({backingFrames, inputFrames, destination.size() / outputChannels});
	for (size_t frame = 0; frame < frames; ++frame) {
		for (uint8_t channel = 0; channel < outputChannels; ++channel) {
			auto backingValue = backing[frame * backingChannels + std::min<uint8_t>(channel, backingChannels - 1)];
			auto inputValue = input[frame * inputChannels + std::min<uint8_t>(channel, inputChannels - 1)];
			destination[frame * outputChannels + channel] = saturatingAdd(backingValue, inputValue);
		}
	}
	return {.accepted = true, .channels = outputChannels};
}

int32_t transposeRatioQ24(int8_t semitones, int8_t cents) {
	auto totalCents = std::clamp<int32_t>(static_cast<int32_t>(semitones) * 100 + cents, -2450, 2450);
	auto ratio = std::exp2(static_cast<double>(totalCents) / 1200.0);
	return static_cast<int32_t>(std::llround(ratio * kTransposeUnity));
}

int32_t GainRamp::gainAt(int64_t tick) const {
	if (endTick <= startTick || tick >= endTick)
		return endGain;
	if (tick <= startTick)
		return startGain;
	auto elapsed = tick - startTick;
	auto duration = endTick - startTick;
	auto delta = static_cast<int64_t>(endGain) - startGain;
	return static_cast<int32_t>(startGain + delta * elapsed / duration);
}

} // namespace deluge::loop_station::core::audio
