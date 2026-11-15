#include "snapshot.h"
#include <limits>
namespace loop_station {
Status SnapshotStore::initialize(std::span<std::byte> storage) {
	return pages_.initialize(storage);
}
const SnapshotStore::Snapshot* SnapshotStore::lookup(SnapshotId id) const {
	if (!id.valid())
		return nullptr;
	for (const auto& snapshot : snapshots_)
		if (snapshot.identity == id.generation)
			return &snapshot;
	return nullptr;
}
bool SnapshotStore::valid(SnapshotId id) const {
	const auto* snapshot = lookup(id);
	if (!snapshot)
		return false;
	// A preserved snapshot owns its record via the pool pin, so it stays valid
	// across a provider rebind (rollback revision change); a plain snapshot still
	// tracks the current rollback mapping.
	if (snapshot->preserved)
		return pool_.length(snapshot->handle) == snapshot->length;
	return pool_.length(snapshot->handle) == snapshot->length
	       && snapshot->revision == rollback_.readRevision(snapshot->handle.slot);
}
Status SnapshotStore::preserve(SlotId slot) {
	if (!validSlot(slot))
		return Status::Invalid;
	auto& snapshot = snapshots_[slot];
	if (!snapshot.identity || snapshot.preserved)
		return Status::Ok;
	if (rollback_.provisional(slot) || rollback_.pending(slot))
		return Status::Busy; // Never freeze an unfinalized gesture mapping.
	if (!valid({snapshot.identity})) {
		invalidate(slot);
		return Status::Stale;
	}
	// Take one bounded record reference so retirement cannot reclaim the source;
	// reads then use the frozen record directly, never a later rollback binding.
	const auto pinned = pool_.pin(snapshot.handle);
	if (pinned != Status::Ok) {
		invalidate(slot);
		return pinned;
	}
	snapshot.preserved = true;
	return Status::Ok;
}
void SnapshotStore::invalidateSlot(SlotId slot) {
	if (validSlot(slot))
		invalidate(slot);
}
void SnapshotStore::releaseAll() {
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		invalidate(slot);
}
void SnapshotStore::invalidate(SlotId slot) {
	auto& snapshot = snapshots_[slot];
	const auto identity = snapshot.identity;
	const auto handle = snapshot.handle;
	const bool pinned = snapshot.preserved;
	snapshot = {}; // Invalidate before returning any pinned pages to the arena.
	if (identity)
		pages_.erase(identity);
	if (pinned)
		pool_.unpin(handle);
}
Status SnapshotStore::request(SlotId slot, Generation generation, SnapshotId& result) {
	result = {};
	if (!pages_.ready() || !validSlot(slot))
		return Status::Invalid;
	const PhraseHandle handle{slot, generation};
	const auto length = pool_.length(handle);
	if (!length || rollback_.boundGeneration(slot) != generation)
		return Status::Stale;
	auto& snapshot = snapshots_[slot];
	if (snapshot.identity && snapshot.handle.generation == generation && valid({snapshot.identity})) {
		result = {snapshot.identity};
		return Status::Ok;
	}
	invalidate(slot);
	if (sequence_ == std::numeric_limits<Generation>::max())
		return Status::Unavailable;
	snapshot.identity = ++sequence_;
	snapshot.handle = handle;
	snapshot.length = length;
	snapshot.revision = rollback_.readRevision(slot);
	result = {snapshot.identity};
	return Status::Ok;
}
Status SnapshotStore::read(SnapshotId id, FrameIndex first, std::span<Frame> output) {
	if (!valid(id))
		return Status::Stale;
	const auto& snapshot = *lookup(id);
	if (output.size() > kStagingFrames || first > snapshot.length || output.size() > snapshot.length - first)
		return Status::Invalid;
	uint32_t lastPage = UINT32_MAX, entry = detail::BeforeImages::none;
	for (size_t i = 0; i < output.size(); ++i) {
		const auto index = first + static_cast<FrameIndex>(i);
		const auto page = index / kStagingFrames;
		if (page != lastPage) {
			entry = pages_.find(snapshot.identity, page);
			lastPage = page;
		}
		if (entry != detail::BeforeImages::none)
			output[i] = pages_.data(entry)[index % kStagingFrames];
		else {
			const auto* source = pool_.frame(snapshot.handle, index);
			if (!source)
				return Status::Stale;
			// A preserved snapshot reads its frozen record directly; a plain one
			// reads the effective original through the current rollback mapping.
			output[i] = snapshot.preserved ? *source : rollback_.read(snapshot.handle.slot, index, *source);
		}
	}
	return valid(id) ? Status::Ok : Status::Stale;
}
Status SnapshotStore::cancel(SnapshotId id) {
	const auto* snapshot = lookup(id);
	if (!snapshot)
		return Status::Stale;
	const auto slot = snapshot->handle.slot;
	invalidate(slot);
	return Status::Ok;
}
void SnapshotStore::release(SnapshotId id) {
	cancel(id);
}
void SnapshotStore::beforeWrite(SlotId slot, FrameIndex index, Frame original) {
	if (!validSlot(slot) || !snapshots_[slot].identity)
		return;
	auto& snapshot = snapshots_[slot];
	if (snapshot.preserved) {
		// A preserved snapshot owns an earlier version. Capture only edits to that
		// same version; a newer same-slot recording must not be folded into it.
		if (pool_.sealedHandle(slot).generation != snapshot.handle.generation)
			return;
	}
	else if (!valid({snapshot.identity})) {
		invalidate(slot);
		return;
	}
	const auto page = index / kStagingFrames;
	if (snapshot.lastPage == page)
		return;
	if (pages_.find(snapshot.identity, page) == detail::BeforeImages::none) {
		const auto entry = pages_.claim(snapshot.identity, page);
		if (entry == detail::BeforeImages::none
		    || !pages_.capture(entry, pool_, rollback_, snapshot.handle, index, original)) {
			invalidate(slot);
			return;
		}
	}
	snapshot.lastPage = page;
}
unsigned SnapshotStore::activeSnapshots() const {
	unsigned count = 0;
	for (const auto& snapshot : snapshots_)
		count += valid({snapshot.identity});
	return count;
}
} // namespace loop_station
