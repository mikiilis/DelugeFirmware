#pragma once

#include "page_pool.h"
#include <optional>

namespace loop_station {

class GestureRollback;
class History;
class SnapshotStore;

enum class CompletionReason : uint8_t { None, GateClosed, Duration, Capacity, OneShot };
struct AudioCompletion {
	CompletionReason reason = CompletionReason::None;
	FrameIndex frames = 0;
	Status status = Status::Ok;
};

class ResidentAudio {
public:
	ResidentAudio(PagePool& pool, GestureRollback& rollback) : pool_(pool), rollback_(rollback) {}
	// Install once before use; callers retain observers for the audio lifetime.
	void setWriteObservers(History* history, SnapshotStore* snapshots) {
		history_ = history;
		snapshots_ = snapshots;
	}
	Status beginInitial(SlotId slot, FrameIndex durationLimit);
	Status finishInitial(SlotId slot, PhraseHandle& handle);
	Status attach(SlotId slot, PhraseHandle handle);
	// Replace a sealed phrase at a serialized boundary while preserving transport.
	Status replace(SlotId slot, PhraseHandle handle);
	Status start(SlotId slot, bool oneShot);
	void stop(SlotId slot);
	Status clear(SlotId slot);
	// Explicit musical stops defer natural completion; cancellation rearms it.
	void setOneShotCompletion(SlotId slot, bool enabled);
	Status setCaptureGate(SlotId slot, bool open);
	// The owner starts one PagePool render round for all manual participants.
	// Output is pre-addition backing. At most min(input.size(), output.size())
	// frames advance; any output tail is silent. No monitoring is mixed here.
	void render(SlotId slot, std::span<const Frame> dryInput, std::span<Frame> dryOutput);
	// After beginRenderRound(): initial-take frames writable without crossing the
	// round's capacity boundary; nullopt for playback/stopped slots. Zero must be
	// serviced before rendering, so no post-capacity window is lost to silence.
	std::optional<FrameIndex> initialCapacity(SlotId slot) const;
	// Admission follows the take begun by beginInitial(), even if settings change.
	uint8_t manualParticipants() const;
	Status serviceCapacity(SlotId slot);
	FrameIndex position(SlotId slot) const;
	Generation contentRevision(SlotId slot) const { return validSlot(slot) ? contentRevisions_[slot] : 0; }
	bool playing(SlotId slot) const { return validSlot(slot) && slots_[slot].playing; }
	// One bounded event per slot. Consume before the next control transition.
	AudioCompletion takeCompletion(SlotId slot);

private:
	struct Slot {
		PhraseHandle handle{};
		FrameIndex length = 0;
		FrameIndex position = 0;
		FrameIndex durationLimit = 0;
		bool initial = false;
		bool initialResult = false;
		bool playing = false;
		bool capture = false;
		bool oneShot = false;
		AudioCompletion completion{};
	};
	void beforeWrite(SlotId slot, FrameIndex index, Frame original);
	void endSession(SlotId slot);
	void contentChanged(SlotId slot) {
		if (validSlot(slot))
			++contentRevisions_[slot];
	}
	Status completeInitial(SlotId slot, CompletionReason reason, Status status);
	PagePool& pool_;
	GestureRollback& rollback_;
	History* history_ = nullptr;
	SnapshotStore* snapshots_ = nullptr;
	Slot slots_[kTrackCount]{};
	std::array<Generation, kTrackCount> contentRevisions_{};
};

} // namespace loop_station
