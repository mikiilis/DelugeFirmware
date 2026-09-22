/* Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "loop_station/loop_station.h"
#include "model/sample/sample_recorder.h"

namespace deluge::loop_station {
// The production Loop Station render boundary. Both spans have one stereo frame
// per output frame. Backing is neutral PCM / 8; physical input is full-scale PCM.
// Record before audible envelope, playback fade, monitoring and pitch processing.
template <typename Pitch>
void renderAudio(std::span<StereoSample> backing, std::span<const StereoSample> input, SampleRecorder* recorder,
                 int32_t amplitudeStart, int32_t amplitudeEnd, int32_t playbackGain, bool monitor, Pitch&& pitch) {
	if (recorder)
		recorder->processLoopOverdub(backing, input);
	int64_t amplitude = amplitudeStart;
	int32_t increment = backing.empty() ? 0 : (int64_t(amplitudeEnd) - amplitudeStart) / int64_t(backing.size());
	for (size_t i = 0; i < backing.size(); ++i) {
		amplitude += increment;
		auto gain = amplitude * playbackGain >> 24;
		backing[i].l = int64_t(backing[i].l) * gain >> 26;
		backing[i].r = int64_t(backing[i].r) * gain >> 26;
		if (monitor) {
			backing[i].l = saturatingAdd(backing[i].l, input[i].l >> kCaptureGainShift);
			backing[i].r = saturatingAdd(backing[i].r, input[i].r >> kCaptureGainShift);
		}
	}
	pitch(backing);
}
} // namespace deluge::loop_station
