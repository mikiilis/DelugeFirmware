#include "transpose.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace loop_station {

Status Transpose::prepare(SlotId slot) {
	if (!validSlot(slot))
		return Status::Invalid;
	auto& state = states_[slot];
	state.prepared = true;
	state.cents = 0;
	state.ratioQ24 = 1u << 24;
	resetStream(state);
	return Status::Ok;
}

void Transpose::setCents(SlotId slot, int32_t totalCents) {
	if (!validSlot(slot) || !states_[slot].prepared)
		return;
	auto& state = states_[slot];
	const auto bounded = std::clamp(totalCents, kMinCents, kMaxCents);
	setCentsAndRatio(slot, bounded, ratioForCentsQ24(bounded));
}

void Transpose::setCentsAndRatio(SlotId slot, int32_t totalCents, uint32_t ratio) {
	if (!validSlot(slot) || !states_[slot].prepared)
		return;
	auto& state = states_[slot];
	state.cents = static_cast<int16_t>(std::clamp(totalCents, kMinCents, kMaxCents));
	state.ratioQ24 = ratio;
}

bool Transpose::process(SlotId slot, std::span<Frame> combinedDry, bool discontinuity) {
	if (!validSlot(slot))
		return false;
	auto& state = states_[slot];
	if (!state.prepared)
		return false;
	if (discontinuity)
		resetStream(state);
	if (state.ratioQ24 == (1u << 24)) {
		// Copy-through is the bit-exact unity path. Do not retain an old grain stream
		// that could reappear when transpose is enabled again later.
		resetStream(state);
		return true;
	}

	for (auto& frame : combinedDry) {
		const Frame input = frame;
		state.history[state.writePosition & kHistoryMask] = input;
		++state.writePosition;
		state.bufferedFrames = std::min<uint32_t>(state.bufferedFrames + 1, kBaseDelayFrames);
		if (!state.active) {
			if (state.bufferedFrames < kBaseDelayFrames)
				continue;
			state.activeHead = 0;
			state.heads[0].positionQ24 = (state.writePosition - kBaseDelayFrames) << 24;
			state.heads[0].age = 0;
			state.heads[0].active = true;
			state.active = true;
		}

		if (!state.crossfading && state.heads[state.activeHead].age >= kHopFrames) {
			const auto nextHead = uint8_t(state.activeHead ^ 1u);
			state.heads[nextHead].positionQ24 = (state.writePosition - kBaseDelayFrames) << 24;
			state.heads[nextHead].age = 0;
			state.heads[nextHead].active = true;
			state.crossfading = true;
			state.crossfadeProgress = 0;
		}

		Frame shifted = readHead(state, state.activeHead);
		if (state.crossfading) {
			const auto nextHead = uint8_t(state.activeHead ^ 1u);
			const Frame next = readHead(state, nextHead);
			shifted = mix(shifted, next, state.crossfadeProgress, kHopFrames);
		}
		if (state.warmupFade < kHopFrames) {
			frame = mix(input, shifted, state.warmupFade, kHopFrames);
			++state.warmupFade;
		}
		else
			frame = shifted;

		for (auto& head : state.heads)
			if (head.active) {
				head.positionQ24 += state.ratioQ24;
				++head.age;
			}
		if (state.crossfading && ++state.crossfadeProgress >= kHopFrames) {
			const auto oldHead = state.activeHead;
			state.activeHead ^= 1u;
			state.heads[oldHead].active = false;
			state.crossfading = false;
			state.crossfadeProgress = 0;
		}
	}
	return true;
}

void Transpose::release(SlotId slot) {
	if (!validSlot(slot))
		return;
	auto& state = states_[slot];
	state.prepared = false;
	resetStream(state);
}

void Transpose::reset(SlotId slot) {
	if (validSlot(slot) && states_[slot].prepared)
		resetStream(states_[slot]);
}

int16_t Transpose::cents(SlotId slot) const {
	return validSlot(slot) ? states_[slot].cents : 0;
}

uint32_t Transpose::ratioQ24(SlotId slot) const {
	return validSlot(slot) ? states_[slot].ratioQ24 : 0;
}

uint32_t Transpose::ratioForCentsQ24(int32_t totalCents) {
	const auto bounded = std::clamp(totalCents, kMinCents, kMaxCents);
	const double ratio = std::pow(2.0, static_cast<double>(bounded) / 1200.0);
	return static_cast<uint32_t>(std::floor(ratio * double(1u << 24) + 0.5));
}

void Transpose::resetStream(State& state) {
	state.heads = {};
	state.writePosition = 0;
	state.bufferedFrames = 0;
	state.warmupFade = 0;
	state.crossfadeProgress = 0;
	state.activeHead = 0;
	state.active = false;
	state.crossfading = false;
}

Frame Transpose::readHead(const State& state, uint8_t headIndex) {
	const auto& head = state.heads[headIndex];
	const auto index = static_cast<uint32_t>(head.positionQ24 >> 24) & kHistoryMask;
	const auto next = (index + 1) & kHistoryMask;
	const auto fraction = static_cast<uint32_t>(head.positionQ24) & 0x00ffffffu;
	const auto interpolate = [fraction](int32_t a, int32_t b) {
		const int64_t delta = int64_t(b) - a;
		return static_cast<int32_t>(std::clamp<int64_t>(int64_t(a) + ((delta * fraction) >> 24),
		                                                std::numeric_limits<int32_t>::min(),
		                                                std::numeric_limits<int32_t>::max()));
	};
	return {interpolate(state.history[index].l, state.history[next].l),
	        interpolate(state.history[index].r, state.history[next].r)};
}

Frame Transpose::mix(Frame a, Frame b, uint32_t amount, uint32_t total) {
	const auto blend = [amount, total](int32_t first, int32_t second) {
		const int64_t mixed = int64_t(first) * (total - amount) + int64_t(second) * amount;
		return static_cast<int32_t>(std::clamp<int64_t>(mixed / total, std::numeric_limits<int32_t>::min(),
		                                                std::numeric_limits<int32_t>::max()));
	};
	return {blend(a.l, b.l), blend(a.r, b.r)};
}

} // namespace loop_station
