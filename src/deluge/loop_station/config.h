#pragma once

#include "types.h"

namespace loop_station {

inline constexpr uint32_t kSampleRate = 44100;
inline constexpr size_t kPageBytes = 4096;
inline constexpr FrameIndex kTenSecondFrames = 10 * kSampleRate;

enum class MemoryProfile : uint8_t { Preferred, TenSecond };
inline constexpr MemoryProfile kConfiguredMemoryProfile = MemoryProfile::Preferred;

constexpr size_t roundUpToPage(size_t bytes) {
	return ((bytes + kPageBytes - 1) / kPageBytes) * kPageBytes;
}

constexpr size_t activeAudioBytes(MemoryProfile profile) {
	switch (profile) {
	case MemoryProfile::Preferred:
		return 33554432;
	case MemoryProfile::TenSecond:
		return kTrackCount * roundUpToPage(kTenSecondFrames * sizeof(Frame));
	}
	return 0;
}

static_assert(activeAudioBytes(MemoryProfile::TenSecond) == 28246016);

} // namespace loop_station
