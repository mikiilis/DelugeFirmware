#pragma once

#include "config.h"

#include <memory>
#include <span>

namespace loop_station {

class PagePool {
public:
	static constexpr unsigned kDetachedRecords = kTrackCount * 2;
	static constexpr uint32_t kMaxPagesPerRecord = 2048;
	// The caller owns storage for the lifetime of the pool. Metadata is allocated here only.
	Status initialize(std::span<std::byte> storage);
	// Call once before processing a render. Bits name all manual slots that may append,
	// including slots whose phrase will begin later. Available pages are granted here.
	Status beginRenderRound(uint8_t manualParticipants);
	// A nonzero reservation claims every required page before recording starts.
	Status beginPhrase(SlotId slot, FrameIndex reservedFrames);
	// Manual append requires participation in the current render round. accepted may
	// be shorter than frames when this slot has claimed its page for the round.
	Status append(SlotId slot, std::span<const Frame> frames, FrameIndex& accepted);
	// Constant-time capacity for the already-selected shared render round. Includes
	// the current partial page and this slot's fair grant, never another slot's.
	FrameIndex appendCapacity(SlotId slot) const;
	Status seal(SlotId slot, PhraseHandle& handle);
	// Foreground restore builder. Detached records are bounded and coexist with
	// every live phrase; they never participate in render-round admission.
	Status beginDetached(SlotId slot, FrameIndex frames, PhraseHandle& handle);
	// Reserve at most maxPages from the shared free stack. Call from a short
	// audio Boundary; detached frame copies can then run outside that Boundary.
	Status reserveDetachedPages(PhraseHandle handle, uint32_t maxPages);
	bool detachedReservationComplete(PhraseHandle handle) const;
	bool canBeginDetached(SlotId slot, FrameIndex frames) const;
	Status writeDetached(PhraseHandle handle, FrameIndex index, Frame frame);
	Status sealDetached(PhraseHandle handle);
	// Constant-time ownership swap. The former live phrase remains detached and
	// is returned for bounded retirement after the boundary commit.
	Status replace(PhraseHandle candidate, PhraseHandle& previous);
	Frame* frame(PhraseHandle handle, FrameIndex index);
	// Returns zero for invalid, unsealed, or stale handles.
	FrameIndex length(PhraseHandle handle) const;
	PhraseHandle sealedHandle(SlotId slot) const {
		const auto record = validSlot(slot) ? activeRecord_[slot] : kRecordCount;
		return record < kRecordCount && slots_[record].owner == slot && slots_[record].sealed
		           ? PhraseHandle{slot, slots_[record].generation}
		           : PhraseHandle{};
	}
	Status release(PhraseHandle handle);
	// O(1) logical invalidation; physical pages are returned incrementally by
	// serviceRetirement and the record cannot be reused until then.
	Status retire(PhraseHandle handle);
	uint32_t serviceRetirement(uint32_t maxPages);
	bool retirementPending() const;
	uint32_t freePages() const;

private:
	std::span<std::byte> storage_{};
	uint32_t capacity_ = 0;
	uint32_t mapStride_ = 0;
	uint32_t freeCount_ = 0;
	std::unique_ptr<uint32_t[]> freeStack_;
	std::unique_ptr<uint32_t[]> pageMap_;
	uint8_t roundParticipants_ = 0;
	uint8_t roundGranted_ = 0;
	uint8_t roundReserved_ = 0;
	SlotId nextGrantSlot_ = 0;
	bool roundHadSpace_ = false;
	uint32_t roundReservedPage_[kTrackCount]{};
	struct Slot {
		SlotId owner = kInvalidSlot;
		Generation generation = 0;
		FrameIndex length = 0;
		FrameIndex reserved = 0;
		uint32_t pages = 0;
		bool active = false, retiring = false;
		bool sealed = false;
	};
	static constexpr unsigned kRecordCount = kTrackCount + kDetachedRecords;
	Slot slots_[kRecordCount]{};
	unsigned activeRecord_[kTrackCount]{};
	Generation sequence_ = 0;
	unsigned retirementCursor_ = 0;
	unsigned findRecord(PhraseHandle) const;
	uint32_t& page(unsigned record, uint32_t ordinal) {
		return pageMap_[static_cast<size_t>(record) * mapStride_ + ordinal];
	}
	uint32_t page(unsigned record, uint32_t ordinal) const {
		return pageMap_[static_cast<size_t>(record) * mapStride_ + ordinal];
	}
};

} // namespace loop_station
