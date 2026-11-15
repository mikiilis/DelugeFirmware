#pragma once
#include "before_images.h"
namespace loop_station {
// All calls are serialized with audio control/render. A snapshot pin is logical:
// untouched pages are read through rollback; first writes copy into this arena.
// No live/preserved pointer escapes. Copy <= kStagingFrames into fixed staging
// before yielding to SD, then recheck valid(id) after EVERY yield and before
// publishing. Staging survives cancellation; a canceled job must not publish it.
class SnapshotStore {
public:
	static constexpr unsigned kMaxSnapshots = kTrackCount;
	static constexpr size_t kStagingFrames = kPageBytes / sizeof(Frame);
	SnapshotStore(PagePool& pool, GestureRollback& rollback) : pool_(pool), rollback_(rollback) {}
	Status initialize(std::span<std::byte> storage);
	Status request(SlotId slot, Generation generation, SnapshotId& result);
	// Freeze a currently valid snapshot across a confirmed history navigation.
	// Pins the source record with the pool and freezes its effective mapping so
	// the acquired version stays readable with its original frames after the
	// provider is detached/replaced. No copy, scan, page or allocation.
	Status preserve(SlotId slot);
	// Explicit destructive invalidation (Clear, unbind, teardown): drops pages and
	// releases any pin so bounded retirement can reclaim the version.
	void invalidateSlot(SlotId slot);
	void releaseAll();
	Status read(SnapshotId id, FrameIndex first, std::span<Frame> output);
	Status cancel(SnapshotId id);
	bool valid(SnapshotId id) const;
	void release(SnapshotId id);
	void beforeWrite(SlotId slot, FrameIndex index, Frame original);
	uint32_t retainedPages() const { return pages_.count(); }
	unsigned activeSnapshots() const;
	const detail::BeforeImages::Work& work() const { return pages_.work(); }

private:
	struct Snapshot {
		Generation identity = 0, revision = 0;
		PhraseHandle handle{};
		FrameIndex length = 0;
		uint32_t lastPage = UINT32_MAX;
		bool preserved = false;
	};
	const Snapshot* lookup(SnapshotId id) const;
	void invalidate(SlotId slot);
	PagePool& pool_;
	GestureRollback& rollback_;
	detail::BeforeImages pages_;
	Snapshot snapshots_[kMaxSnapshots]{};
	Generation sequence_ = 0;
};
} // namespace loop_station
