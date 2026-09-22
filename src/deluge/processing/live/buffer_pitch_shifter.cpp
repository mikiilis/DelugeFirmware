/* Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "processing/live/buffer_pitch_shifter.h"
#include <algorithm>
#include <new>

// The optional legacy repitched cache reads RX directly and allocates at hop
// boundaries. Buffer-fed processing must never enable that path.
static_assert(!INPUT_ENABLE_REPITCHED_BUFFER);

void BufferPitchShifter::render(std::span<StereoSample> audio, int32_t phaseIncrement, uint32_t time) {
	if (audio.empty())
		return;
	// Unity is an exact bypass, including when the user returns transpose to zero.
	// Reset heads only after a real gap or bypass, never on a monitoring refresh.
	bool reset = bypassed_ || input_.upToTime != time;
	input_.giveInput(audio, time);
	if (phaseIncrement == kMaxSampleValue) {
		bypassed_ = true;
		return;
	}
	if (reset) {
		shifter_.~LivePitchShifter();
		new (&shifter_) LivePitchShifter(OscType::INPUT_STEREO, phaseIncrement);
		shifter_.playHeads[PLAY_HEAD_NEWER].rawBufferReadPos =
		    (input_.numRawSamplesProcessed - audio.size()) & (kInputRawBufferSize - 1);
	}
	bypassed_ = false;
	std::fill(audio.begin(), audio.end(), StereoSample{});
	// The buffer already has audible gain applied. LivePitchShifter's direct
	// reader multiplies by amplitude >> 28, so this is unity output gain.
	shifter_.render(reinterpret_cast<int32_t*>(audio.data()), audio.size(), phaseIncrement, 1 << 28, 0,
	                kInterpolationMaxNumSamples, &input_);
}
