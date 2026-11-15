#pragma once

#include "config.h"

#include <cstdio>
#include <numeric>

namespace loop_station {
inline void formatPhraseLength(FrameIndex frames, uint32_t tempoQ16, char* out, size_t size) {
	if (!size)
		return;

	if (frames && tempoQ16 && tempoQ16 <= 1000U * 65536U) {
		constexpr uint64_t framesPerBarQ16 = uint64_t(kSampleRate) * 240 * 65536;
		constexpr uint32_t grids[] = {32, 48};
		for (const auto grid : grids) {
			// These products fit uint64_t for every FrameIndex and validated tempo.
			const uint64_t scaled = uint64_t(frames) * tempoQ16 * grid;
			const uint64_t units = (scaled + framesPerBarQ16 / 2) / framesPerBarQ16;
			if (!units)
				continue;
			const uint64_t numerator = units * framesPerBarQ16;
			const uint64_t denominator = uint64_t(tempoQ16) * grid;
			const uint64_t expected = numerator / denominator + (numerator % denominator != 0);
			const uint64_t difference = expected > frames ? expected - frames : frames - expected;
			if (difference > 1)
				continue;

			const auto divisor = std::gcd(units, uint64_t(grid));
			const auto bars = static_cast<unsigned>(units / divisor);
			const auto fraction = static_cast<unsigned>(grid / divisor);
			char label[32];
			const char* suffix = bars <= fraction ? "" : "S";
			const int length = fraction == 1
			                       ? std::snprintf(label, sizeof(label), "%u BAR%s", bars, suffix)
			                       : std::snprintf(label, sizeof(label), "%u/%u BAR%s", bars, fraction, suffix);
			if (length > 0 && length <= 8) {
				std::snprintf(out, size, "%s", label);
				return;
			}
			break; // An oversized musical label uses seconds, not a truncated fraction.
		}
	}

	const uint64_t hundredths = (uint64_t(frames) * 100 + kSampleRate / 2) / kSampleRate;
	if (frames && !hundredths)
		std::snprintf(out, size, "<0.01 s");
	else if (hundredths < 100000)
		std::snprintf(out, size, "%u.%02u s", static_cast<unsigned>(hundredths / 100),
		              static_cast<unsigned>(hundredths % 100));
	else
		std::snprintf(out, size, "%u s", static_cast<unsigned>((uint64_t(frames) + kSampleRate / 2) / kSampleRate));
}
} // namespace loop_station
