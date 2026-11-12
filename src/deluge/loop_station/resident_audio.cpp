#include "resident_audio.h"
#include "gesture_rollback.h"
#include "history.h"
#include "snapshot.h"

#include <algorithm>
#include <limits>

namespace loop_station {
namespace {
int32_t addSaturated(int32_t backing, int32_t input) {
	const int64_t sum = static_cast<int64_t>(backing) + input;
	return static_cast<int32_t>(
	    std::clamp<int64_t>(sum, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}
} // namespace

Status ResidentAudio::beginInitial(SlotId slot, FrameIndex durationLimit) {
	if (!validSlot(slot) || !rollback_.ready())
		return Status::Invalid;
	if (rollback_.provisional(slot) || rollback_.pending(slot))
		return Status::Busy;
	const auto status = pool_.beginPhrase(slot, durationLimit);
	if (status != Status::Ok)
		return status;
	auto& state = slots_[slot];
	state = {};
	state.durationLimit = durationLimit;
	state.initial = true;
	state.capture = true;
	return Status::Ok;
}

Status ResidentAudio::completeInitial(SlotId slot, CompletionReason reason, Status status) {
	auto& state = slots_[slot];
	auto sealed = pool_.seal(slot, state.handle);
	if (sealed == Status::Ok)
		sealed = rollback_.bind(slot, state.handle);
	state.initial = false;
	state.initialResult = sealed == Status::Ok;
	state.capture = false;
	state.position = 0;
	state.completion = {reason, state.length, status == Status::Ok ? sealed : status};
	if (sealed == Status::Ok && state.length)
		contentChanged(slot);
	return sealed;
}

Status ResidentAudio::finishInitial(SlotId slot, PhraseHandle& handle) {
	handle = {};
	if (!validSlot(slot))
		return Status::Invalid;
	auto& state = slots_[slot];
	if (state.initial)
		completeInitial(slot, CompletionReason::GateClosed, Status::Ok);
	if (!state.initialResult)
		return Status::Invalid;
	if (pool_.length(state.handle) == 0)
		return Status::Stale;
	handle = state.handle;
	return Status::Ok;
}

Status ResidentAudio::attach(SlotId slot, PhraseHandle handle) {
	if (!validSlot(slot) || !handle.valid() || handle.slot != slot)
		return Status::Invalid;
	if (slots_[slot].initial)
		return Status::Busy;
	const auto length = pool_.length(handle);
	if (length == 0)
		return Status::Stale;
	const auto status = rollback_.bind(slot, handle);
	if (status != Status::Ok)
		return status;
	endSession(slot);
	slots_[slot] = {};
	slots_[slot].handle = handle;
	slots_[slot].length = length;
	return Status::Ok;
}

Status ResidentAudio::replace(SlotId slot, PhraseHandle handle) {
	if (!validSlot(slot) || !handle.valid() || handle.slot != slot)
		return Status::Invalid;
	auto& state = slots_[slot];
	if (state.initial || state.capture || rollback_.provisional(slot) || rollback_.pending(slot))
		return Status::Busy;
	const auto length = pool_.length(handle);
	if (!length)
		return Status::Stale;
	const auto status = rollback_.bind(slot, handle);
	if (status != Status::Ok)
		return status;
	endSession(slot);
	state.handle = handle;
	state.length = length;
	state.position = state.playing ? state.position % length : 0;
	state.capture = false;
	state.initial = false;
	state.initialResult = false;
	state.completion = {};
	contentChanged(slot);
	if (history_)
		history_->mappingReplaced(slot);
	return Status::Ok;
}

Status ResidentAudio::start(SlotId slot, bool oneShot) {
	if (!validSlot(slot))
		return Status::Invalid;
	auto& state = slots_[slot];
	if (state.initial)
		return Status::Busy;
	if (!state.handle.valid())
		return Status::Invalid;
	if (pool_.length(state.handle) == 0)
		return Status::Stale;
	const auto status = rollback_.bind(slot, state.handle);
	if (status != Status::Ok)
		return status;
	endSession(slot);
	state.position = 0;
	state.playing = true;
	state.capture = false;
	state.oneShot = oneShot;
	state.completion = {};
	return Status::Ok;
}

void ResidentAudio::beforeWrite(SlotId slot, FrameIndex index, Frame original) {
	contentChanged(slot);
	rollback_.beforeWrite(slot, index, original);
	if (history_)
		history_->beforeWrite(slot, index, original);
	if (snapshots_)
		snapshots_->beforeWrite(slot, index, original);
}
void ResidentAudio::endSession(SlotId slot) {
	if (history_)
		history_->endSession(slot);
}

void ResidentAudio::stop(SlotId slot) {
	if (!validSlot(slot))
		return;
	auto& state = slots_[slot];
	if (state.initial)
		completeInitial(slot, CompletionReason::GateClosed, Status::Ok);
	endSession(slot);
	state.playing = false;
	state.capture = false;
}

void ResidentAudio::setOneShotCompletion(SlotId slot, bool enabled) {
	if (validSlot(slot))
		slots_[slot].oneShot = enabled;
}

Status ResidentAudio::clear(SlotId slot) {
	if (!validSlot(slot))
		return Status::Invalid;
	auto& state = slots_[slot];
	if (state.initial || state.capture || state.playing || rollback_.provisional(slot) || rollback_.pending(slot))
		return Status::Busy;
	if (state.handle.valid()) {
		auto status = pool_.retire(state.handle);
		if (status != Status::Ok)
			return status;
	}
	state = {};
	contentChanged(slot);
	if (history_)
		history_->resetSlot(slot);
	return Status::Ok;
}

Status ResidentAudio::setCaptureGate(SlotId slot, bool open) {
	if (!validSlot(slot))
		return Status::Invalid;
	auto& state = slots_[slot];
	if (state.initial && !open)
		return completeInitial(slot, CompletionReason::GateClosed, Status::Ok);
	if (!state.initial && !state.playing)
		return Status::Invalid;
	if (!state.initial && open != state.capture && history_) {
		if (open)
			history_->beginSession(slot, state.handle.generation);
		else
			endSession(slot);
	}
	state.capture = open;
	return Status::Ok;
}

void ResidentAudio::render(SlotId slot, std::span<const Frame> dryInput, std::span<Frame> dryOutput) {
	const size_t count = std::min(dryInput.size(), dryOutput.size());
	if (!validSlot(slot)) {
		std::fill(dryOutput.begin(), dryOutput.end(), Frame{});
		return;
	}
	auto& state = slots_[slot];
	if (state.initial) {
		const auto wanted = state.durationLimit ? std::min<size_t>(count, state.durationLimit - state.length) : count;
		if (wanted != 0) {
			FrameIndex accepted = 0;
			const auto status = pool_.append(slot, dryInput.first(wanted), accepted);
			state.length += accepted;
			if (state.durationLimit && state.length == state.durationLimit)
				completeInitial(slot, CompletionReason::Duration, Status::Ok);
			else if (accepted != wanted || status != Status::Ok)
				completeInitial(slot, CompletionReason::Capacity, status == Status::Ok ? Status::Busy : status);
		}
		std::fill(dryOutput.begin(), dryOutput.end(), Frame{});
		return;
	}

	size_t cursor = 0;
	while (cursor < count && state.playing) {
		Frame* backing = pool_.frame(state.handle, state.position);
		if (!backing) {
			endSession(slot);
			state.playing = false;
			state.capture = false;
			break;
		}
		constexpr FrameIndex framesPerPage = kPageBytes / sizeof(Frame);
		const size_t span = std::min<size_t>(
		    {count - cursor, state.length - state.position, framesPerPage - state.position % framesPerPage});
		for (size_t i = 0; i < span; ++i) {
			const Frame input = dryInput[cursor + i];
			const auto index = state.position + static_cast<FrameIndex>(i);
			const Frame original = rollback_.read(slot, index, backing[i]);
			dryOutput[cursor + i] = original;
			if (state.capture) {
				beforeWrite(slot, index, original);
				backing[i] = {addSaturated(original.l, input.l), addSaturated(original.r, input.r)};
			}
			rollback_.rendered(slot);
		}
		cursor += span;
		state.position += static_cast<FrameIndex>(span);
		if (state.position == state.length) {
			state.position = 0;
			if (state.oneShot && !state.capture) {
				state.playing = false;
				state.completion = {CompletionReason::OneShot, state.length, Status::Ok};
			}
		}
	}
	std::fill(dryOutput.begin() + cursor, dryOutput.end(), Frame{});
}

uint8_t ResidentAudio::manualParticipants() const {
	uint8_t participants = 0;
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		if (slots_[slot].initial && !slots_[slot].durationLimit)
			participants |= 1u << slot;
	return participants;
}

std::optional<FrameIndex> ResidentAudio::initialCapacity(SlotId slot) const {
	if (!validSlot(slot) || !slots_[slot].initial)
		return std::nullopt;
	return pool_.appendCapacity(slot);
}

Status ResidentAudio::serviceCapacity(SlotId slot) {
	if (!validSlot(slot) || !slots_[slot].initial || slots_[slot].durationLimit || pool_.appendCapacity(slot) != 0)
		return Status::Invalid;
	return completeInitial(slot, CompletionReason::Capacity, Status::NoMemory);
}

FrameIndex ResidentAudio::position(SlotId slot) const {
	return validSlot(slot) ? slots_[slot].position : 0;
}

AudioCompletion ResidentAudio::takeCompletion(SlotId slot) {
	if (!validSlot(slot))
		return {};
	const auto completion = slots_[slot].completion;
	slots_[slot].completion = {};
	return completion;
}

} // namespace loop_station
