/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "definitions_cxx.hpp"

namespace deluge::dsp {

class InputPitchShifter {
public:
	InputPitchShifter();
	void setPitchRatio(float ratio);
	void process(std::span<StereoSample> buffer);
	void processMono(std::span<q31_t> buffer);

private:
	float pitchRatio_ = 1.0f;
	float phase_ = 0.0f;
	StereoSample prevSample_{};
	StereoSample currSample_{};
	q31_t prevSampleMono_ = 0;
	q31_t currSampleMono_ = 0;
	bool firstSample_ = true;
};

} // namespace deluge::dsp