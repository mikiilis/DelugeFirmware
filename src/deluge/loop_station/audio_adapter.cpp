#include "audio_adapter.h"

#include <algorithm>
#include <limits>

namespace loop_station {
namespace {
constexpr uint8_t channels(Source source) {
	return source == Source::Left ? 1 : source == Source::Right ? 2 : 3;
}
Frame route(Frame input, Source source) {
	switch (source) {
	case Source::Left:
		return {input.l, input.l};
	case Source::Right:
		return {input.r, input.r};
	case Source::Balanced: {
		// Matches SampleRecorder and AudioOutput, including signed odd samples.
		const auto mono = input.l / 2 - input.r / 2;
		return {mono, mono};
	}
	case Source::Stereo:
		return input;
	}
	return {};
}
int32_t add(int32_t a, int32_t b) {
	return static_cast<int32_t>(std::clamp<int64_t>(int64_t(a) + b, INT32_MIN, INT32_MAX));
}
int32_t monitorLevel(int32_t value, int64_t gain) {
	// Preserve unity exactly; other gains follow Q31 monitoring convention.
	return gain == INT32_MAX ? value : static_cast<int32_t>((int64_t(value) * gain) >> 31);
}
} // namespace

void AudioAdapter::finalizeWindow(SampleClock now, size_t& frames, const ClockMap& clock) {
	engine_.service(now, clock);
	if (const auto next = engine_.nextActionSample(clock); next && *next > now)
		frames = static_cast<size_t>(std::min<uint64_t>(frames, *next - now));
	windowFrames_ = frames;
	rendered_ = 0;
	if (!frames)
		return;
	pool_.beginRenderRound(audio_.manualParticipants());
	bool capacityCompletion = false;
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		if (const auto capacity = audio_.initialCapacity(slot); capacity && *capacity == 0) {
			audio_.serviceCapacity(slot);
			capacityCompletion = true;
		}
	}
	// Seal at the boundary and enter Play/Dub before a single subsequent sample.
	// This consumes completions for ALL slots together without another grant round.
	if (capacityCompletion)
		engine_.service(now, clock);
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		const bool playing = engine_.state(slot).playing;
		const auto generation = engine_.streamGeneration(slot);
		if ((playing && !lastPlaying_[slot]) || generation != lastStreamGeneration_[slot])
			transpose_.reset(slot);
		lastPlaying_[slot] = playing;
		lastStreamGeneration_[slot] = generation;
	}
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		if (const auto capacity = audio_.initialCapacity(slot); capacity && *capacity != 0)
			frames = std::min<size_t>(frames, *capacity);
	windowFrames_ = frames;
}

bool AudioAdapter::monitors(SlotId slot) const {
	if (!validSlot(slot) || engine_.settings().tracks[slot].monitoring != Monitoring::On)
		return false;
	const auto state = engine_.state(slot);
	if (state.capture)
		return true;
	if (slot != engine_.selected())
		return false;
	const auto source = channels(engine_.settings().tracks[slot].source);
	for (SlotId other = 0; other < kTrackCount; ++other)
		if (engine_.state(other).capture && (source & channels(engine_.settings().tracks[other].source)))
			return false;
	return true;
}

bool AudioAdapter::consumesPhysicalInput() const {
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		if (engine_.state(slot).capture || monitors(slot))
			return true;
	return false;
}

void AudioAdapter::renderRouted(SlotId slot, size_t count) {
	const auto source = engine_.settings().tracks[slot].source;
	for (size_t i = 0; i < count; ++i)
		input_[i] = route(input_[i], source);
	auto backing = std::span(output_[slot]).first(count);
	audio_.render(slot, std::span<const Frame>(input_).first(count), backing);
	if (monitors(slot)) {
		for (size_t i = 0; i < count; ++i) {
			const auto offset = static_cast<int64_t>(std::min(i + 1, windowFrames_));
			const int64_t gain =
			    monitorStart_ == monitorEnd_ || !windowFrames_
			        ? monitorEnd_
			        : monitorStart_
			              + (int64_t(monitorEnd_) - monitorStart_) * offset / static_cast<int64_t>(windowFrames_);
			backing[i] = {add(backing[i].l, monitorLevel(input_[i].l, gain)),
			              add(backing[i].r, monitorLevel(input_[i].r, gain))};
		}
	}
	// The output path is already a single backing-plus-monitor stream here.
	// ResidentAudio has captured the dry physical input inside audio_.render();
	// applying this stage now keeps capture dry and lets the output's usual FX
	// process the transposed combined signal exactly once.
	transpose_.process(slot, backing);
	renderedFrames_[slot] = count;
	rendered_ |= 1u << slot;
}

void AudioAdapter::renderDry(SlotId slot, std::span<const Frame> physicalInput, std::span<Frame> output) {
	if (!validSlot(slot)) {
		std::fill(output.begin(), output.end(), Frame{});
		return;
	}
	if (!(rendered_ & (1u << slot))) {
		const auto count = std::min({physicalInput.size(), output.size(), input_.size()});
		std::copy_n(physicalInput.begin(), count, input_.begin());
		renderRouted(slot, count);
	}
	const auto count = std::min(output.size(), renderedFrames_[slot]);
	std::copy_n(output_[slot].begin(), count, output.begin());
	std::fill(output.begin() + count, output.end(), Frame{});
}

FrameIndex AudioAdapter::phase(SlotId slot) const {
	return audio_.position(slot);
}

} // namespace loop_station
