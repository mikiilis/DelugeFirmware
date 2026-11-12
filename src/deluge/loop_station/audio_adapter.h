#pragma once

#include "engine.h"
#include "transpose.h"

struct StereoSample;

namespace loop_station {

// Borrowed, serialized audio-boundary view of the song owner's engine and storage.
// The owner must outlive this adapter and all bound clips, install/remove it between
// callbacks, and update ClockMap from the song tempo/metronome/transport. None of
// those clock updates change resident phrase phase. No UI or fixture is involved.
class AudioAdapter {
public:
	static constexpr size_t kMaxWindowFrames = 128;
	AudioAdapter(Engine& engine, ResidentAudio& audio, PagePool& pool, Transpose& transpose)
	    : engine_(engine), audio_(audio), pool_(pool), transpose_(transpose) {}
	// Call once per device render window, after ordinary tick dispatch, even when
	// transport is stopped. Services due actions before splitting at future ones
	// and starts ONE shared manual-admission round for every slot in the window.
	void finalizeWindow(SampleClock now, size_t& frames, const ClockMap& clock);
	// Physical codec-native signed 32-bit frames. Capture stays in that convention;
	// output is pre-addition backing plus the independently arbitrated monitor.
	// Device windows are <= kMaxWindowFrames. Repeated source renders reuse the
	// dry window without advancing/capturing twice, before output FX/level/pan.
	void renderDry(SlotId slot, std::span<const Frame> physicalInput, std::span<Frame> output);
	void renderDry(SlotId slot, std::span<const ::StereoSample> physicalInput, std::span<::StereoSample> output);
	FrameIndex phase(SlotId slot) const;
	bool monitors(SlotId slot) const;
	bool consumesPhysicalInput() const;
	// Codec power-on monitoring fade; never attenuates the captured input/backing.
	void setMonitoringGain(int32_t start, int32_t end) {
		monitorStart_ = start;
		monitorEnd_ = end;
	}

private:
	Engine& engine_;
	ResidentAudio& audio_;
	PagePool& pool_;
	Transpose& transpose_;
	void renderRouted(SlotId slot, size_t count);
	std::array<Frame, kMaxWindowFrames> input_;
	std::array<std::array<Frame, kMaxWindowFrames>, kTrackCount> output_;
	std::array<size_t, kTrackCount> renderedFrames_{};
	std::array<bool, kTrackCount> lastPlaying_{};
	std::array<uint32_t, kTrackCount> lastStreamGeneration_{};
	uint8_t rendered_ = 0;
	size_t windowFrames_ = 0;
	int32_t monitorStart_ = INT32_MAX, monitorEnd_ = INT32_MAX;
};

} // namespace loop_station
