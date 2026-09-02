/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "loop_station/model/types.h"

#include <array>
#include <cstdint>
#include <span>

namespace deluge::loop_station::core::audio {

struct RecorderRequest {
	SlotId slot = SlotId::ONE;
	uint8_t channels = 1;
};

class RecorderBudget {
public:
	static constexpr uint8_t kChannelCapacity = 16;
	[[nodiscard]] bool reserve(RecorderRequest request);
	void release(SlotId slot);
	[[nodiscard]] uint8_t used() const { return used_; }
	[[nodiscard]] uint8_t available() const { return kChannelCapacity - used_; }

private:
	std::array<uint8_t, kSlotCount> reservations_{};
	uint8_t used_ = 0;
};

struct RouteRequest {
	SlotId slot = SlotId::ONE;
	PhysicalSource source = PhysicalSource::LEFT;
	uint32_t processingSignature = 0;
	bool capturing = false;
	bool selected = false;
	bool monitoring = false;
};

class PhysicalInputRouter {
public:
	void clear();
	void subscribe(RouteRequest request);
	[[nodiscard]] bool shouldRenderSource(PhysicalSource source) const;
	[[nodiscard]] bool monitorAudible(SlotId slot) const;
	[[nodiscard]] size_t sourceReadCount() const;
	[[nodiscard]] size_t monitorReturnCount() const;

private:
	std::array<RouteRequest, kSlotCount> routes_{};
	std::array<bool, kSlotCount> used_{};
};

[[nodiscard]] constexpr int32_t saturatingAdd(int32_t backing, int32_t input) {
	auto total = static_cast<int64_t>(backing) + input;
	if (total > INT32_MAX) return INT32_MAX;
	if (total < INT32_MIN) return INT32_MIN;
	return static_cast<int32_t>(total);
}

struct DryMixResult {
	bool accepted = true;
	uint8_t channels = 1;
};

// Interleaved buffers. Mono is duplicated only when the resulting revision is
// stereo. A missing backing buffer rejects the whole provisional revision.
DryMixResult mixDryCumulative(std::span<int32_t const> backing, uint8_t backingChannels,
	                          std::span<int32_t const> input, uint8_t inputChannels,
	                          std::span<int32_t> destination, bool backingRequired = true);

[[nodiscard]] int32_t transposeRatioQ24(int8_t semitones, int8_t cents);
[[nodiscard]] constexpr bool transposeBypasses(int8_t semitones, int8_t cents) {
	return semitones == 0 && cents == 0;
}

struct GainRamp {
	int64_t startTick = 0;
	int64_t endTick = 0;
	int32_t startGain = 0;
	int32_t endGain = kTransposeUnity;

	[[nodiscard]] int32_t gainAt(int64_t tick) const;
};

} // namespace deluge::loop_station::core::audio
