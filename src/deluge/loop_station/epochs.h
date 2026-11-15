#pragma once
#include "types.h"
#include <limits>

namespace loop_station {
// Monotonic identity epochs for navigation and transport. An epoch saturates into
// an explicit exhausted sentinel instead of wrapping: once exhausted it cannot
// re-authorize an expired identity token, and callers must refuse further
// navigation rather than continuing with a saturated value. These are pure
// production utilities so the near-maximum boundary is testable without a
// mutable production knob.
inline constexpr Generation kExhaustedEpoch = std::numeric_limits<Generation>::max();
inline constexpr uint32_t kExhaustedEpoch32 = std::numeric_limits<uint32_t>::max();

constexpr Generation nextEpoch(Generation current) {
	return current >= kExhaustedEpoch ? kExhaustedEpoch : current + 1;
}

constexpr uint32_t nextEpoch32(uint32_t current) {
	return current >= kExhaustedEpoch32 ? kExhaustedEpoch32 : current + 1;
}

constexpr bool epochExhausted(Generation current) {
	return current >= kExhaustedEpoch;
}

constexpr bool epochExhausted32(uint32_t current) {
	return current >= kExhaustedEpoch32;
}

constexpr bool navigationEpochsAvailable(Generation history, Generation transport) {
	return !epochExhausted(history) && !epochExhausted(transport);
}
} // namespace loop_station
