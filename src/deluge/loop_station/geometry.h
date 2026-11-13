#pragma once

#include "types.h"

namespace loop_station {

enum class PadRegion : uint8_t { Invalid, Centre, Border };

struct PadAddress {
	SlotId slot = kInvalidSlot;
	PadRegion region = PadRegion::Invalid;
	uint8_t borderIndex = 0;
	bool valid = false;
};

enum class EntryDecision : uint8_t { Ignore, Enter, FeatureDisabled, ArrangementActive };
constexpr EntryDecision crossScreenEntryDecision(bool pressed, bool idleSongRoot, bool shiftHeld,
                                                 bool arrangementActive, bool featureEnabled) {
	if (!pressed || !idleSongRoot || shiftHeld)
		return EntryDecision::Ignore;
	if (arrangementActive)
		return EntryDecision::ArrangementActive;
	return featureEnabled ? EntryDecision::Enter : EntryDecision::FeatureDisabled;
}

// Physical 16 by 8 coordinates: y=0 is bottom, y=7 is top. Each 4 by 4 tile is one track,
// numbered left-to-right across the top row, then the bottom row.
PadAddress padAddress(int32_t x, int32_t y);

// Number of clockwise border pads filled for the current native sample frame.
uint8_t filledBorderPads(FrameIndex position, FrameIndex phraseFrames);

// Red clockwise sweep toward the two-second stopped-track clear gesture.
uint8_t clearHoldSweepPads(SampleClock heldSamples, SampleClock holdDurationSamples);

// Stable visual phase for pending/error pulses, independent of audio tempo.
bool visualPulseOn(SampleClock sample, SampleClock halfPeriodSamples = 11025);

} // namespace loop_station
