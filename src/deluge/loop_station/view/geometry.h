#pragma once

#include "loop_station/model/types.h"

#include <array>

namespace deluge::loop_station::core::view {

struct Pad {
	uint8_t x = 0;
	uint8_t y = 0;
	constexpr bool operator==(Pad const&) const = default;
};

class Geometry {
public:
	static constexpr size_t kBorderPads = 12;
	static constexpr std::array<Pad, kBorderPads> kClockwiseBorder{
	    {{0, 3}, {1, 3}, {2, 3}, {3, 3}, {3, 2}, {3, 1}, {3, 0}, {2, 0}, {1, 0}, {0, 0}, {0, 1}, {0, 2}}};
	[[nodiscard]] static constexpr Pad origin(SlotId slot) {
		auto i = static_cast<uint8_t>(slot);
		return {static_cast<uint8_t>((i % 4) * 4), static_cast<uint8_t>((1 - i / 4) * 4)};
	}
	[[nodiscard]] static constexpr SlotId slotAt(uint8_t x, uint8_t y) {
		return static_cast<SlotId>((1 - y / 4) * 4 + x / 4);
	}
	[[nodiscard]] static constexpr bool centre(uint8_t x, uint8_t y) {
		return x % 4 >= 1 && x % 4 <= 2 && y % 4 >= 1 && y % 4 <= 2;
	}
	[[nodiscard]] static uint8_t progressPad(int64_t phase, int64_t phraseLength);
	[[nodiscard]] static uint16_t progressInterpolation(int64_t phaseQ16, int64_t phraseLength);
};

// The four physical LEDs in a centre are one logical button. Only the first
// down and final up are emitted, preventing finger-rolls from creating takes.
class CentreCoalescer {
public:
	struct Event {
		bool down = false;
		bool up = false;
		SlotId slot = SlotId::ONE;
	};
	[[nodiscard]] Event update(uint8_t x, uint8_t y, bool pressed);
	void reset();

private:
	std::array<uint8_t, kSlotCount> held_{};
};

} // namespace deluge::loop_station::core::view
