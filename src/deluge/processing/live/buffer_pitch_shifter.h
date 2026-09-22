/* Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "processing/live/live_input_buffer.h"
#include "processing/live/live_pitch_shifter.h"

// One private stereo input history per track. Allocate outside the audio callback.
class BufferPitchShifter {
public:
	BufferPitchShifter() : shifter_(OscType::INPUT_STEREO, kMaxSampleValue) {}
	void render(std::span<StereoSample> audio, int32_t phaseIncrement, uint32_t time);

private:
	LivePitchShifter shifter_;
	bool bypassed_ = true;
	LiveInputBuffer input_;
	// LiveInputBuffer's final rawBuffer is intentionally extended for stereo,
	// as in AudioEngine::getOrCreateLiveInputBuffer().
	int32_t stereoExtension_[kInputRawBufferSize]{};
};
