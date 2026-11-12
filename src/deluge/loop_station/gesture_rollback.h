#pragma once

#include "page_pool.h"

namespace loop_station {

// All methods run on the serialized audio owner, at sample boundaries. Returned
// frames are values: no reader retains an arena pointer across a control change.
class GestureRollback {
public:
	static constexpr uint32_t kProvisionalSamples = 15435;
	// Expiry is checked per sample; no dispatch-window padding is required.
	static constexpr uint16_t kCapacity = kProvisionalSamples;
	struct Work {
		uint64_t capturedFrames = 0, restoredFrames = 0, retiredFrames = 0, treeSteps = 0;
	};

	explicit GestureRollback(PagePool& pool) : pool_(pool) {}
	static constexpr size_t requiredBytes();
	// Storage belongs to the caller and remains at a fixed address for this lifetime.
	Status initialize(std::span<std::byte> storage);
	bool ready() const { return initialized_; }
	// Changes only when phrase identity or effective read mapping changes.
	Generation readRevision(SlotId slot) const { return validSlot(slot) ? slots_[slot].revision : 0; }
	Generation boundGeneration(SlotId slot) const { return validSlot(slot) ? slots_[slot].handle.generation : 0; }
	Status bind(SlotId slot, PhraseHandle handle);
	Status begin(SlotId slot, SampleClock now, FrameIndex phase, FrameIndex phraseFrames);
	// original is the effective frame returned by read, before ordinary history.
	void beforeWrite(SlotId slot, FrameIndex index, Frame original);
	Frame read(SlotId slot, FrameIndex index, Frame backing) const;
	Status confirm(SlotId slot);
	Status cancel(SlotId slot);
	// Engine observes the authoritative clock before dispatch and service. Older
	// observations cannot move a slot clock backwards. Render advances each slot
	// once per processed sample, so even an oversized render expires exactly.
	void advance(SampleClock now);
	void rendered(SlotId slot);
	// Each credit retires at most one entry, fairly across slots. One credit is
	// also mandatory before each write; new gestures never wait for background IO.
	void service(uint32_t maxFrames);
	// Pending entries still refer to the bound phrase; retire before replacement.
	bool pending(SlotId slot) const;
	bool provisional(SlotId slot) const;
	const Work& work() const { return work_; }

private:
	using Index = uint16_t;
	static constexpr Index nil = UINT16_MAX;
	enum class Resolution : uint8_t { Active, Cancelled, Confirmed };
	struct Entry {
		Frame original{};
		FrameIndex key = 0;
		Index left = nil, right = nil, previous = nil, next = nil, owner = nil;
		uint8_t height = 1;
	};
	struct Descriptor {
		Index count = 0, next = nil;
		Resolution resolution = Resolution::Active;
	};
	struct Slot {
		Entry* entries = nullptr;
		Descriptor* descriptors = nullptr;
		PhraseHandle handle{};
		FrameIndex length = 0;
		Generation revision = 0;
		SampleClock start = 0, clock = 0;
		Index root = nil, freeEntry = 0, freeDescriptor = 0, active = nil;
		Index activeHead = nil, activeTail = nil, retiredHead = nil, retiredTail = nil;
	};
	Index find(const Slot& slot, FrameIndex key) const;
	Index insert(Slot& slot, Index root, Index entry);
	Index erase(Slot& slot, Index root, FrameIndex key);
	Index extractMinimum(Slot& slot, Index root, Index& minimum);
	Index balance(Slot& slot, Index root);
	Index rotateLeft(Slot& slot, Index root);
	Index rotateRight(Slot& slot, Index root);
	static int height(const Slot& slot, Index entry);
	static void updateHeight(Slot& slot, Index entry);
	static void append(Slot& slot, Index entry, Index& head, Index& tail);
	static void unlink(Slot& slot, Index entry, Index& head, Index& tail);
	void releaseOwner(Slot& slot, Index owner);
	void removeRetired(Slot& slot, Index entry, bool restore);
	void retireOne(Slot& slot);
	Status resolve(SlotId slot, Resolution resolution);
	PagePool& pool_;
	Slot slots_[kTrackCount]{};
	SlotId nextService_ = 0;
	bool initialized_ = false;
	mutable Work work_{};
};

constexpr size_t GestureRollback::requiredBytes() {
	return kTrackCount * (kCapacity * sizeof(Entry) + (kCapacity + 1) * sizeof(Descriptor));
}

} // namespace loop_station
