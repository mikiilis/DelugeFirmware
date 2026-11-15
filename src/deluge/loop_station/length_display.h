#pragma once

#include "config.h"

#include <charconv>
#include <cstring>
#include <numeric>
#include <string_view>

namespace loop_station {
namespace length_display_detail {
// Largest intermediate label is well below 32 bytes. Keep formatting independent
// of printf: the embedded libc implementation pulls in unavailable OS syscalls.
class Label {
public:
	void append(std::string_view text) {
		for (char c : text) {
			if (length_ < sizeof(data_))
				data_[length_++] = c;
		}
	}
	void number(unsigned value) {
		char digits[10]; // Enough for every uint32_t value.
		const auto result = std::to_chars(digits, digits + sizeof(digits), value);
		append(std::string_view(digits, result.ptr - digits));
	}
	size_t size() const { return length_; }
	void copyTo(char* out, size_t size) const {
		if (!size)
			return;
		const size_t count = length_ < size - 1 ? length_ : size - 1;
		std::memcpy(out, data_, count);
		out[count] = '\0';
	}

private:
	char data_[32]{};
	size_t length_ = 0;
};
} // namespace length_display_detail

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
			length_display_detail::Label label;
			label.number(bars);
			if (fraction != 1) {
				label.append("/");
				label.number(fraction);
			}
			label.append(bars <= fraction ? " BAR" : " BARS");
			if (label.size() <= 8) {
				label.copyTo(out, size);
				return;
			}
			break; // An oversized musical label uses seconds, not a truncated fraction.
		}
	}

	const uint64_t hundredths = (uint64_t(frames) * 100 + kSampleRate / 2) / kSampleRate;
	length_display_detail::Label label;
	if (frames && !hundredths)
		label.append("<0.01 s");
	else if (hundredths < 100000) {
		label.number(static_cast<unsigned>(hundredths / 100));
		label.append(hundredths % 100 < 10 ? ".0" : ".");
		label.number(static_cast<unsigned>(hundredths % 100));
		label.append(" s");
	}
	else {
		label.number(static_cast<unsigned>((uint64_t(frames) + kSampleRate / 2) / kSampleRate));
		label.append(" s");
	}
	label.copyTo(out, size);
}
} // namespace loop_station
