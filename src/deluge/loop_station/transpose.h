#pragma once

#include "types.h"
#include <array>
#include <span>

namespace loop_station {

class Transpose {
public:
	static constexpr int32_t kMinCents = -2450;
	static constexpr int32_t kMaxCents = 2450;

	Status prepare(SlotId slot);
	void setCents(SlotId slot, int32_t totalCents);
	void setCentsAndRatio(SlotId slot, int32_t totalCents, uint32_t ratioQ24);
	bool process(SlotId slot, std::span<Frame> combinedDry, bool discontinuity = false);
	void reset(SlotId slot);
	void release(SlotId slot);
	int16_t cents(SlotId slot) const;
	uint32_t ratioQ24(SlotId slot) const;
	static uint32_t ratioForCentsQ24(int32_t totalCents);

private:
	static constexpr uint32_t kHistoryFrames = 4096;
	static constexpr uint32_t kHistoryMask = kHistoryFrames - 1;
	static constexpr uint32_t kHopFrames = 256;
	static constexpr uint32_t kBaseDelayFrames = 2304;

	struct Head {
		uint64_t positionQ24 = 0;
		uint32_t age = 0;
		bool active = false;
	};
	struct State {
		std::array<Frame, kHistoryFrames> history{};
		std::array<Head, 2> heads{};
		uint64_t writePosition = 0;
		uint32_t ratioQ24 = 1u << 24;
		uint32_t bufferedFrames = 0, warmupFade = 0, crossfadeProgress = 0;
		uint8_t activeHead = 0;
		int16_t cents = 0;
		bool prepared = false, active = false, crossfading = false;
	};
	std::array<State, kTrackCount> states_{};
	static void resetStream(State& state);
	static Frame readHead(const State& state, uint8_t headIndex);
	static Frame mix(Frame a, Frame b, uint32_t amount, uint32_t total);
};

} // namespace loop_station
