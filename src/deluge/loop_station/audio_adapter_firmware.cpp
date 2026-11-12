#include "audio_adapter.h"
#include "dsp/stereo_sample.h"
#include <algorithm>

namespace loop_station {

void AudioAdapter::renderDry(SlotId slot, std::span<const ::StereoSample> physicalInput,
                             std::span<::StereoSample> output) {
	static_assert(SSI_TX_BUFFER_NUM_SAMPLES == kMaxWindowFrames);
	if (!validSlot(slot)) {
		std::fill(output.begin(), output.end(), ::StereoSample{});
		return;
	}
	// Value copies avoid aliasing the independent pure/firmware sample types.
	// Scratch/cache belongs to the preallocated owner, not the callback stack.
	if (!(rendered_ & (1u << slot))) {
		const auto count = std::min({physicalInput.size(), output.size(), input_.size()});
		for (size_t i = 0; i < count; ++i)
			input_[i] = {physicalInput[i].l, physicalInput[i].r};
		renderRouted(slot, count);
	}
	const auto count = std::min(output.size(), renderedFrames_[slot]);
	for (size_t i = 0; i < count; ++i)
		output[i] = {output_[slot][i].l, output_[slot][i].r};
	std::fill(output.begin() + count, output.end(), ::StereoSample{});
}

} // namespace loop_station
