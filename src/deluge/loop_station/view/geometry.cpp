#include "loop_station/view/geometry.h"

#include <algorithm>

namespace deluge::loop_station::core::view {

uint8_t Geometry::progressPad(int64_t phase, int64_t phraseLength) {
	if (phraseLength <= 0) return 0;
	auto local = floorMod(phase, phraseLength);
	return std::min<uint8_t>(kBorderPads - 1, local * kBorderPads / phraseLength);
}

uint16_t Geometry::progressInterpolation(int64_t phaseQ16, int64_t phraseLength) {
	if (phraseLength <= 0) return 0;
	auto phraseQ16 = phraseLength << 16;
	auto local = floorMod(phaseQ16, phraseQ16);
	auto padQ16 = local * kBorderPads / phraseLength;
	return static_cast<uint16_t>(padQ16 & 0xffff);
}

CentreCoalescer::Event CentreCoalescer::update(uint8_t x, uint8_t y, bool pressed) {
	if (x >= 16 || y >= 8 || !Geometry::centre(x, y)) return {};
	auto slot = Geometry::slotAt(x, y);
	auto& held = held_[indexOf(slot)];
	if (pressed) {
		if (held < 4) ++held;
		return {.down = held == 1, .slot = slot};
	}
	if (held == 0) return {};
	--held;
	return {.up = held == 0, .slot = slot};
}

void CentreCoalescer::reset() { held_.fill(0); }

} // namespace deluge::loop_station::core::view
