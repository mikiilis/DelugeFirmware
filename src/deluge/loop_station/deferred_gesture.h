/*
 * Copyright © 2026 Deluge Community Firmware contributors
 *
 * This file is part of the Deluge Firmware and is licensed under GPL-3.0-or-later.
 */

#pragma once

#include <cstdint>
#include <optional>

namespace deluge::loop_station {

enum class DeferredGlobalButton : uint8_t { LOAD, SAVE };
enum class DeferredGestureKind : uint8_t;

struct DeferredGlobalGesture {
	DeferredGlobalButton button = DeferredGlobalButton::LOAD;
	uint32_t pressSample = 0;
	std::optional<uint32_t> releaseSample;
	bool shiftHeld = false;

	[[nodiscard]] constexpr uint32_t heldSamples() const { return releaseSample ? *releaseSample - pressSample : 0; }

	[[nodiscard]] constexpr DeferredGestureKind classify(uint32_t shortPressThreshold) const;
};

enum class DeferredGestureKind : uint8_t { SHORT_PRESS, LONG_PRESS, STILL_HELD, CLEAR_SONG, IGNORE };

constexpr DeferredGestureKind DeferredGlobalGesture::classify(uint32_t shortPressThreshold) const {
	if (shiftHeld)
		return button == DeferredGlobalButton::LOAD ? DeferredGestureKind::CLEAR_SONG : DeferredGestureKind::IGNORE;
	if (!releaseSample)
		return DeferredGestureKind::STILL_HELD;
	return heldSamples() < shortPressThreshold ? DeferredGestureKind::SHORT_PRESS : DeferredGestureKind::LONG_PRESS;
}

class DeferredGlobalGestureQueue {
public:
	[[nodiscard]] bool press(DeferredGlobalButton button, uint32_t sample, bool shiftHeld) {
		if (pending_)
			return false;
		pending_ = DeferredGlobalGesture{.button = button, .pressSample = sample, .shiftHeld = shiftHeld};
		return true;
	}

	[[nodiscard]] bool release(DeferredGlobalButton button, uint32_t sample) {
		if (!pending_ || pending_->button != button || pending_->releaseSample)
			return false;
		pending_->releaseSample = sample;
		return true;
	}

	[[nodiscard]] std::optional<DeferredGlobalGesture> take() {
		auto result = pending_;
		pending_.reset();
		return result;
	}

	[[nodiscard]] bool pending() const { return pending_.has_value(); }
	void clear() { pending_.reset(); }

private:
	std::optional<DeferredGlobalGesture> pending_;
};

} // namespace deluge::loop_station
