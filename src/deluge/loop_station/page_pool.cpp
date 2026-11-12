#include "page_pool.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace loop_station {

namespace {
constexpr uint32_t kFramesPerPage = kPageBytes / sizeof(Frame);
static_assert(kPageBytes % sizeof(Frame) == 0);

uint32_t pagesFor(FrameIndex frames) {
	return static_cast<uint32_t>((static_cast<uint64_t>(frames) + kFramesPerPage - 1) / kFramesPerPage);
}
} // namespace

Status PagePool::initialize(std::span<std::byte> storage) {
	if (capacity_ != 0 || storage.empty() || storage.size() % kPageBytes != 0
	    || reinterpret_cast<uintptr_t>(storage.data()) % alignof(Frame) != 0
	    || storage.size() / kPageBytes > std::numeric_limits<uint32_t>::max() / kRecordCount)
		return Status::Invalid;
	const auto capacity = static_cast<uint32_t>(storage.size() / kPageBytes);
	const auto mapStride = std::min(capacity, kMaxPagesPerRecord);
	auto freeStack = std::unique_ptr<uint32_t[]>(new (std::nothrow) uint32_t[capacity]);
	auto pageMap =
	    std::unique_ptr<uint32_t[]>(new (std::nothrow) uint32_t[static_cast<size_t>(mapStride) * kRecordCount]);
	if (!freeStack || !pageMap)
		return Status::NoMemory;
	for (uint32_t page = 0; page < capacity; ++page)
		freeStack[page] = capacity - page - 1;
	storage_ = storage;
	capacity_ = capacity;
	mapStride_ = mapStride;
	freeCount_ = capacity;
	freeStack_ = std::move(freeStack);
	pageMap_ = std::move(pageMap);
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		slots_[slot].owner = slot;
		activeRecord_[slot] = slot;
	}
	return Status::Ok;
}

unsigned PagePool::findRecord(PhraseHandle handle) const {
	if (!handle.valid())
		return kRecordCount;
	const auto active = activeRecord_[handle.slot];
	if (active < kRecordCount && !slots_[active].retiring && slots_[active].sealed
	    && slots_[active].generation == handle.generation)
		return active;
	for (unsigned i = 0; i < kRecordCount; ++i)
		if (i != active && slots_[i].owner == handle.slot && slots_[i].generation == handle.generation
		    && slots_[i].sealed && !slots_[i].retiring)
			return i;
	return kRecordCount;
}

Status PagePool::beginRenderRound(uint8_t manualParticipants) {
	if (!capacity_)
		return Status::Invalid;
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		if (roundReserved_ & (1u << slot))
			freeStack_[freeCount_++] = roundReservedPage_[slot];
	roundParticipants_ = manualParticipants;
	roundGranted_ = 0;
	roundReserved_ = 0;
	roundHadSpace_ = freeCount_ != 0;
	const SlotId firstSlot = nextGrantSlot_;
	for (SlotId checked = 0; checked < kTrackCount && freeCount_ != 0; ++checked) {
		const SlotId slot = (firstSlot + checked) % kTrackCount;
		const uint8_t bit = 1u << slot;
		if (manualParticipants & bit) {
			roundReservedPage_[slot] = freeStack_[--freeCount_];
			roundGranted_ |= bit;
			roundReserved_ |= bit;
			nextGrantSlot_ = (slot + 1) % kTrackCount;
		}
	}
	return Status::Ok;
}

Status PagePool::beginPhrase(SlotId slot, FrameIndex reservedFrames) {
	if (!capacity_ || !validSlot(slot))
		return Status::Invalid;
	unsigned record = activeRecord_[slot];
	const bool selectRecord = record >= kRecordCount;
	if (selectRecord) {
		record = kRecordCount;
		for (unsigned candidate = 0; candidate < kRecordCount; ++candidate) {
			if (slots_[candidate].active || slots_[candidate].sealed || slots_[candidate].retiring
			    || slots_[candidate].pages || slots_[candidate].owner != kInvalidSlot)
				continue;
			bool referenced = false;
			for (SlotId owner = 0; owner < kTrackCount; ++owner)
				referenced |= activeRecord_[owner] == candidate;
			if (!referenced) {
				record = candidate;
				break;
			}
		}
		if (record == kRecordCount)
			return Status::NoMemory;
	}
	auto& phrase = slots_[record];
	if (phrase.active || phrase.sealed)
		return Status::Busy;
	const auto needed = pagesFor(reservedFrames);
	if (needed > mapStride_ || needed > freeCount_)
		return Status::NoMemory;
	if (selectRecord)
		activeRecord_[slot] = record;
	phrase.generation = ++sequence_;
	if (phrase.generation == 0)
		phrase.generation = ++sequence_;
	phrase.owner = slot;
	phrase.active = true;
	phrase.length = 0;
	phrase.reserved = reservedFrames;
	phrase.pages = 0;
	for (uint32_t ordinal = 0; ordinal < needed; ++ordinal)
		page(record, phrase.pages++) = freeStack_[--freeCount_];
	return Status::Ok;
}

FrameIndex PagePool::appendCapacity(SlotId slot) const {
	if (!validSlot(slot) || activeRecord_[slot] >= kRecordCount || !slots_[activeRecord_[slot]].active)
		return 0;
	const auto& phrase = slots_[activeRecord_[slot]];
	if (phrase.reserved)
		return phrase.reserved - phrase.length;
	if (!(roundParticipants_ & (1u << slot)))
		return 0;
	return phrase.pages * kFramesPerPage - phrase.length + ((roundReserved_ & (1u << slot)) ? kFramesPerPage : 0);
}

Status PagePool::append(SlotId slot, std::span<const Frame> frames, FrameIndex& accepted) {
	accepted = 0;
	if (!validSlot(slot) || activeRecord_[slot] >= kRecordCount || !slots_[activeRecord_[slot]].active)
		return Status::Invalid;
	const unsigned record = activeRecord_[slot];
	auto& phrase = slots_[record];
	if (phrase.reserved == 0 && !(roundParticipants_ & (1u << slot)))
		return Status::Busy;
	while (accepted < frames.size() && phrase.length < std::numeric_limits<FrameIndex>::max()) {
		const uint32_t pageIndex = phrase.length / kFramesPerPage;
		if (pageIndex == phrase.pages) {
			if (phrase.pages >= mapStride_)
				return Status::NoMemory;
			if (phrase.reserved)
				break;
			const uint8_t bit = 1u << slot;
			if (!(roundReserved_ & bit)) {
				if (roundGranted_ & bit)
					break;
				return roundHadSpace_ ? Status::Busy : Status::NoMemory;
			}
			page(record, phrase.pages++) = roundReservedPage_[slot];
			roundReserved_ &= ~bit;
		}
		const auto available = kFramesPerPage - phrase.length % kFramesPerPage;
		const auto wanted = frames.size() - accepted;
		const auto withinReservation =
		    phrase.reserved ? phrase.reserved - phrase.length : std::numeric_limits<FrameIndex>::max() - phrase.length;
		const auto count = static_cast<FrameIndex>(std::min<size_t>({available, wanted, withinReservation}));
		if (count == 0)
			break;
		auto* destination =
		    reinterpret_cast<Frame*>(storage_.data() + static_cast<size_t>(page(record, pageIndex)) * kPageBytes);
		std::memcpy(destination + phrase.length % kFramesPerPage, frames.data() + accepted,
		            static_cast<size_t>(count) * sizeof(Frame));
		phrase.length += count;
		accepted += count;
	}
	return Status::Ok;
}

Status PagePool::seal(SlotId slot, PhraseHandle& handle) {
	handle = {};
	if (!validSlot(slot) || activeRecord_[slot] >= kRecordCount || !slots_[activeRecord_[slot]].active)
		return Status::Invalid;
	const unsigned record = activeRecord_[slot];
	auto& phrase = slots_[record];
	if (phrase.length == 0) {
		for (uint32_t page = 0; page < phrase.pages; ++page)
			freeStack_[freeCount_++] = this->page(record, page);
		phrase.pages = 0;
		phrase.active = false;
		return Status::Invalid;
	}
	phrase.active = false;
	phrase.sealed = true;
	handle = {slot, phrase.generation};
	return Status::Ok;
}

Frame* PagePool::frame(PhraseHandle handle, FrameIndex index) {
	const auto record = findRecord(handle);
	if (record == kRecordCount)
		return nullptr;
	const auto& phrase = slots_[record];
	if (index >= phrase.length)
		return nullptr;
	const auto physical = page(record, index / kFramesPerPage);
	return reinterpret_cast<Frame*>(storage_.data() + static_cast<size_t>(physical) * kPageBytes)
	       + index % kFramesPerPage;
}

FrameIndex PagePool::length(PhraseHandle handle) const {
	const auto record = findRecord(handle);
	return record == kRecordCount ? 0 : slots_[record].length;
}

Status PagePool::release(PhraseHandle handle) {
	auto record = findRecord(handle);
	if (record == kRecordCount && handle.valid())
		for (unsigned i = kTrackCount; i < kRecordCount; ++i)
			if (slots_[i].owner == handle.slot && slots_[i].generation == handle.generation && slots_[i].active) {
				record = i;
				break;
			}
	if (record == kRecordCount)
		return Status::Stale;
	auto& phrase = slots_[record];
	for (uint32_t page = 0; page < phrase.pages; ++page)
		freeStack_[freeCount_++] = this->page(record, page);
	phrase.pages = 0;
	phrase.length = 0;
	phrase.sealed = false;
	phrase.owner = kInvalidSlot;
	if (activeRecord_[handle.slot] == record)
		activeRecord_[handle.slot] = kRecordCount;
	phrase.active = false;
	phrase.reserved = 0;
	phrase.retiring = false;
	return Status::Ok;
}

Status PagePool::retire(PhraseHandle handle) {
	if (!handle.valid())
		return Status::Invalid;
	for (unsigned record = 0; record < kRecordCount; ++record) {
		auto& phrase = slots_[record];
		if (phrase.owner == handle.slot && phrase.generation == handle.generation && !phrase.retiring
		    && (phrase.sealed || phrase.active)) {
			phrase.retiring = true;
			phrase.sealed = false;
			phrase.active = false;
			if (activeRecord_[handle.slot] == record)
				activeRecord_[handle.slot] = kRecordCount;
			return Status::Ok;
		}
	}
	return Status::Stale;
}

uint32_t PagePool::serviceRetirement(uint32_t maxPages) {
	uint32_t released = 0;
	for (unsigned checked = 0; checked < kRecordCount && released < maxPages; ++checked) {
		const unsigned record = retirementCursor_++ % kRecordCount;
		auto& phrase = slots_[record];
		if (!phrase.retiring)
			continue;
		while (phrase.pages && released < maxPages) {
			const auto ordinal = --phrase.pages;
			freeStack_[freeCount_++] = page(record, ordinal);
			++released;
		}
		if (!phrase.pages) {
			phrase = {};
			phrase.owner = kInvalidSlot;
		}
	}
	return released;
}

bool PagePool::retirementPending() const {
	for (const auto& phrase : slots_)
		if (phrase.retiring)
			return true;
	return false;
}

Status PagePool::beginDetached(SlotId slot, FrameIndex frames, PhraseHandle& handle) {
	handle = {};
	if (!capacity_ || !validSlot(slot) || frames == 0)
		return Status::Invalid;
	unsigned record = kRecordCount;
	for (unsigned i = kTrackCount; i < kRecordCount; ++i) {
		bool referenced = false;
		for (SlotId owner = 0; owner < kTrackCount; ++owner)
			referenced |= activeRecord_[owner] == i;
		if (!referenced && slots_[i].owner == kInvalidSlot && !slots_[i].active && !slots_[i].sealed) {
			record = i;
			break;
		}
	}
	if (record == kRecordCount)
		return Status::NoMemory;
	const auto needed = pagesFor(frames);
	if (needed > mapStride_)
		return Status::NoMemory;
	auto& phrase = slots_[record];
	phrase = {};
	phrase.owner = slot;
	phrase.generation = ++sequence_;
	if (!phrase.generation)
		phrase.generation = ++sequence_;
	phrase.active = true;
	phrase.reserved = frames;
	handle = {slot, phrase.generation};
	return Status::Ok;
}

Status PagePool::reserveDetachedPages(PhraseHandle handle, uint32_t maxPages) {
	if (!handle.valid() || !maxPages)
		return Status::Invalid;
	unsigned record = kRecordCount;
	for (unsigned i = kTrackCount; i < kRecordCount; ++i)
		if (slots_[i].owner == handle.slot && slots_[i].generation == handle.generation && slots_[i].active) {
			record = i;
			break;
		}
	if (record == kRecordCount)
		return Status::Stale;
	auto& phrase = slots_[record];
	const auto needed = pagesFor(phrase.reserved);
	if (needed > mapStride_)
		return Status::NoMemory;
	const auto remaining = needed - phrase.pages;
	if (remaining > freeCount_)
		return Status::NoMemory;
	const auto count = std::min<uint32_t>({remaining, maxPages, freeCount_});
	for (uint32_t i = 0; i < count; ++i)
		page(record, phrase.pages++) = freeStack_[--freeCount_];
	return phrase.pages == needed ? Status::Ok : Status::Busy;
}

bool PagePool::detachedReservationComplete(PhraseHandle handle) const {
	if (!handle.valid())
		return false;
	for (unsigned i = kTrackCount; i < kRecordCount; ++i) {
		const auto& phrase = slots_[i];
		if (phrase.owner == handle.slot && phrase.generation == handle.generation && phrase.active)
			return phrase.pages == pagesFor(phrase.reserved);
	}
	return false;
}

bool PagePool::canBeginDetached(SlotId slot, FrameIndex frames) const {
	if (!capacity_ || !validSlot(slot) || frames == 0 || pagesFor(frames) > mapStride_ || pagesFor(frames) > freeCount_)
		return false;
	for (unsigned i = kTrackCount; i < kRecordCount; ++i)
		if (slots_[i].owner == kInvalidSlot && !slots_[i].active && !slots_[i].sealed)
			return true;
	return false;
}

Status PagePool::writeDetached(PhraseHandle handle, FrameIndex index, Frame value) {
	if (!handle.valid())
		return Status::Invalid;
	unsigned record = kRecordCount;
	for (unsigned i = kTrackCount; i < kRecordCount; ++i)
		if (slots_[i].owner == handle.slot && slots_[i].generation == handle.generation && slots_[i].active) {
			record = i;
			break;
		}
	if (record == kRecordCount)
		return Status::Stale;
	auto& phrase = slots_[record];
	if (index != phrase.length || index >= phrase.reserved || index / kFramesPerPage >= phrase.pages)
		return Status::Invalid;
	auto* destination = reinterpret_cast<Frame*>(
	    storage_.data() + static_cast<size_t>(page(record, index / kFramesPerPage)) * kPageBytes);
	destination[index % kFramesPerPage] = value;
	++phrase.length;
	return Status::Ok;
}

Status PagePool::sealDetached(PhraseHandle handle) {
	if (!handle.valid())
		return Status::Invalid;
	for (unsigned i = kTrackCount; i < kRecordCount; ++i) {
		auto& phrase = slots_[i];
		if (phrase.owner == handle.slot && phrase.generation == handle.generation && phrase.active) {
			if (phrase.length != phrase.reserved || phrase.length == 0)
				return Status::Invalid;
			phrase.active = false;
			phrase.sealed = true;
			return Status::Ok;
		}
	}
	return Status::Stale;
}

Status PagePool::replace(PhraseHandle candidate, PhraseHandle& previous) {
	previous = {};
	const auto record = findRecord(candidate);
	if (record == kRecordCount || !validSlot(candidate.slot) || activeRecord_[candidate.slot] == record)
		return Status::Stale;
	const auto active = activeRecord_[candidate.slot];
	if (active >= kRecordCount || slots_[active].owner != candidate.slot)
		return Status::Unavailable;
	if (slots_[active].sealed)
		previous = {candidate.slot, slots_[active].generation};
	else if (slots_[active].pages || slots_[active].length || slots_[active].active)
		return Status::Unavailable;
	activeRecord_[candidate.slot] = record;
	return Status::Ok;
}

uint32_t PagePool::freePages() const {
	return freeCount_;
}

} // namespace loop_station
