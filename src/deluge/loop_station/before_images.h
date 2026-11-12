#pragma once
#include "gesture_rollback.h"
#include <algorithm>

namespace loop_station::detail {
// Fixed metadata; caller-owned page bytes, disjoint from live/rollback/other
// observer arenas. Each owner is a non-reused identity. Subpage storage is
// accepted as zero capacity; bytes beyond kMaxPages are deliberately unused.
class BeforeImages {
public:
	struct Work {
		uint64_t copiedPages = 0, copiedFrames = 0;
	};
	const Work& work() const { return work_; }
	static constexpr uint32_t kMaxPages = 1024;
	static constexpr uint32_t none = UINT32_MAX;
	Status initialize(std::span<std::byte> storage) {
		if (ready_)
			return Status::Busy;
		if (!storage.empty() && reinterpret_cast<uintptr_t>(storage.data()) % alignof(Frame))
			return Status::Invalid;
		capacity_ = static_cast<uint32_t>(std::min<size_t>(storage.size() / kPageBytes, kMaxPages));
		storage_ = storage.first(capacity_ * kPageBytes);
		ready_ = true;
		return Status::Ok;
	}
	bool ready() const { return ready_; }
	uint32_t count() const { return count_; }
	uint32_t capacity() const { return capacity_; }
	uint32_t find(Generation owner, uint32_t page) const {
		for (uint32_t i = 0; i < capacity_; ++i)
			if (entries_[i].owner == owner && entries_[i].page == page)
				return i;
		return none;
	}
	uint32_t claim(Generation owner, uint32_t page) {
		for (uint32_t i = 0; i < capacity_; ++i)
			if (!entries_[i].owner) {
				entries_[i] = {owner, page};
				++count_;
				return i;
			}
		return none;
	}
	void erase(Generation owner) {
		for (uint32_t i = 0; i < capacity_; ++i)
			if (entries_[i].owner == owner) {
				entries_[i] = {};
				--count_;
			}
	}
	uint32_t eraseStep(Generation owner, uint32_t& cursor, uint32_t maxEntries) {
		uint32_t processed = 0;
		while (cursor < capacity_ && processed < maxEntries) {
			auto& entry = entries_[cursor++];
			if (entry.owner == owner) {
				entry = {};
				--count_;
			}
			++processed;
		}
		return processed;
	}
	Frame* data(uint32_t entry) { return reinterpret_cast<Frame*>(storage_.data() + entry * kPageBytes); }
	const Frame* data(uint32_t entry) const {
		return reinterpret_cast<const Frame*>(storage_.data() + entry * kPageBytes);
	}
	bool capture(uint32_t entry, PagePool& pool, GestureRollback& rollback, PhraseHandle handle, FrameIndex index,
	             Frame original) {
		constexpr FrameIndex perPage = kPageBytes / sizeof(Frame);
		const FrameIndex first = index / perPage * perPage;
		const auto length = pool.length(handle);
		if (!length || index >= length)
			return false;
		const auto count = std::min(perPage, length - first);
		auto* source = pool.frame(handle, first);
		if (!source)
			return false;
		auto* target = data(entry);
		for (FrameIndex i = 0; i < count; ++i)
			target[i] = rollback.read(handle.slot, first + i, source[i]);
		target[index - first] = original;
		++work_.copiedPages;
		work_.copiedFrames += count;
		return true;
	}

private:
	struct Entry {
		Generation owner = 0;
		uint32_t page = 0;
	};
	Work work_{};
	Entry entries_[kMaxPages]{};
	std::span<std::byte> storage_{};
	uint32_t capacity_ = 0, count_ = 0;
	bool ready_ = false;
};
} // namespace loop_station::detail
