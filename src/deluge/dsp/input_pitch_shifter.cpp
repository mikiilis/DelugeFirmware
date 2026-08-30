/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dsp/input_pitch_shifter.h"

#include <algorithm>

namespace deluge::dsp {

InputPitchShifter::InputPitchShifter() = default;

void InputPitchShifter::setPitchRatio(float ratio) {
	pitchRatio_ = std::clamp(ratio, 0.25f, 4.0f); // Limit to ±2 octaves
	phase_ = 0.0f;
	firstSample_ = true;
}

void InputPitchShifter::process(std::span<StereoSample> buffer) {
	if (pitchRatio_ == 1.0f) {
		return; // No pitch shifting needed
	}

	if (firstSample_) {
		// Initialize with first input sample
		currSample_ = buffer[0];
		prevSample_ = buffer[0];
		firstSample_ = false;
	}

	for (StereoSample& sample : buffer) {
		// Linear interpolation between prev and curr samples
		float frac = phase_ - static_cast<int32_t>(phase_);
		int32_t index = static_cast<int32_t>(phase_);

		StereoSample interpolated;
		if (index == 0) {
			interpolated.l = static_cast<q31_t>(prevSample_.l + frac * (currSample_.l - prevSample_.l));
			interpolated.r = static_cast<q31_t>(prevSample_.r + frac * (currSample_.r - prevSample_.r));
		}
		else {
			// For simplicity, just use currSample when phase >= 1
			// In a more complete implementation, we'd maintain a circular buffer
			interpolated = currSample_;
		}

		sample = interpolated;
		phase_ += pitchRatio_;

		if (phase_ >= 1.0f) {
			phase_ -= 1.0f;
			prevSample_ = currSample_;
			currSample_ = sample; // This will be overwritten next iteration
		}
	}
}

void InputPitchShifter::processMono(std::span<q31_t> buffer) {
	if (pitchRatio_ == 1.0f) {
		return; // No pitch shifting needed
	}

	if (firstSample_) {
		currSampleMono_ = buffer[0];
		prevSampleMono_ = buffer[0];
		firstSample_ = false;
	}

	for (q31_t& sample : buffer) {
		float frac = phase_ - static_cast<int32_t>(phase_);
		int32_t index = static_cast<int32_t>(phase_);

		q31_t interpolated;
		if (index == 0) {
			interpolated = static_cast<q31_t>(prevSampleMono_ + frac * (currSampleMono_ - prevSampleMono_));
		}
		else {
			interpolated = currSampleMono_;
		}

		sample = interpolated;
		phase_ += pitchRatio_;

		if (phase_ >= 1.0f) {
			phase_ -= 1.0f;
			prevSampleMono_ = currSampleMono_;
			currSampleMono_ = sample;
		}
	}
}

} // namespace deluge::dsp