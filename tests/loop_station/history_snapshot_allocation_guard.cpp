#include "deluge/loop_station/gesture_rollback.h"
#include "deluge/loop_station/history.h"
#include "deluge/loop_station/resident_audio.h"
#include "deluge/loop_station/snapshot.h"
#include <cstdlib>
#include <new>
static unsigned allocationCalls = 0;
void* operator new(std::size_t size) {
	++allocationCalls;
	if (auto* p = std::malloc(size))
		return p;
	throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
	return ::operator new(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
	++allocationCalls;
	return std::malloc(size);
}
void operator delete(void* p) noexcept {
	std::free(p);
}
void operator delete[](void* p) noexcept {
	std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
	std::free(p);
}
int main() {
	using namespace loop_station;
	alignas(Frame) static std::byte live[16 * kPageBytes]{};
	alignas(Frame) static std::byte mandatory[GestureRollback::requiredBytes()]{};
	alignas(Frame) static std::byte historyBytes[65 * kPageBytes]{};
	alignas(Frame) static std::byte snapshotBytes[4 * kPageBytes]{};
	PagePool pool;
	GestureRollback rollback(pool);
	History history(pool, rollback);
	SnapshotStore snapshots(pool, rollback);
	ResidentAudio audio(pool, rollback);
	audio.setWriteObservers(&history, &snapshots);
	if (pool.initialize(live) != Status::Ok)
		return 1;
	const auto before = allocationCalls;
	if (rollback.initialize(mandatory) != Status::Ok || history.initialize(historyBytes) != Status::Ok
	    || snapshots.initialize(snapshotBytes) != Status::Ok)
		return 2;
	PhraseHandle handles[kTrackCount];
	static Frame initial[513]{};
	Frame output[513];
	for (SlotId s = 0; s < kTrackCount; ++s) {
		if (audio.beginInitial(s, 513) != Status::Ok)
			return 3;
		audio.render(s, initial, output);
		if (audio.finishInitial(s, handles[s]) != Status::Ok || audio.start(s, false) != Status::Ok)
			return 4;
	}
	// Initial-boundary pressure across all slots: Undo-to-empty retains the live
	// recording, Redo republishes it, and the fixed descriptor budget holds.
	auto drive = [&](SlotId s, bool redo) -> Status {
		RestoreId id;
		auto status = history.requestRestore(s, redo, id);
		if (status != Status::Ok)
			return status;
		for (unsigned i = 0; i < PagePool::kMaxPagesPerRecord / 8 + 1; ++i) {
			status = history.reserveRestore(id, 8);
			if (status != Status::Busy)
				break;
		}
		if (status != Status::Ok)
			return status;
		status = Status::Busy;
		for (unsigned i = 0; i < 100000 && status == Status::Busy; ++i)
			status = history.serviceRestore(id, 64);
		if (status != Status::Ok)
			return status;
		return history.commitRestore(id);
	};
	for (SlotId s = 0; s < kTrackCount; ++s) {
		if (drive(s, false) != Status::Ok || pool.sealedHandle(s).valid())
			return 14;
		if (audio.detachForHistory(s) != Status::Ok)
			return 15;
		history.mappingReplaced(s);
		if (drive(s, true) != Status::Ok)
			return 16;
		const auto republished = pool.sealedHandle(s);
		if (!republished.valid() || audio.replace(s, republished) != Status::Ok || audio.start(s, false) != Status::Ok)
			return 17;
		if (pool.frame(pool.sealedHandle(s), 0) == nullptr) // Live audio is never reclaimed.
			return 18;
		if (history.retainedSessions() > History::kMaxSessions || history.retainedPages() > 65)
			return 19;
	}
	const Frame input{1, -1};
	Frame staging[SnapshotStore::kStagingFrames];
	SnapshotId previous[kTrackCount]{};
	for (unsigned boundary = 0; boundary < 100000; ++boundary) {
		const auto s = SlotId(boundary % kTrackCount);
		SnapshotId id;
		if (snapshots.request(s, handles[s].generation, id) != Status::Ok)
			return 5;
		if (snapshots.valid(previous[s]))
			return 6;
		if (audio.setCaptureGate(s, true) != Status::Ok)
			return 7;
		if (rollback.begin(s, boundary, audio.position(s), 513) != Status::Ok)
			return 8;
		audio.render(s, {&input, 1}, {output, 1});
		if (boundary % 2 ? rollback.confirm(s) != Status::Ok : rollback.cancel(s) != Status::Ok)
			return 9;
		if (audio.setCaptureGate(s, false) != Status::Ok)
			return 10;
		auto status = snapshots.read(id, 0, staging);
		if (status != Status::Ok && status != Status::Stale)
			return 11;
		snapshots.release(id);
		previous[s] = id;
		rollback.service(8);
		if (snapshots.retainedPages() > 4 || snapshots.activeSnapshots() > kTrackCount || history.retainedPages() > 65
		    || history.retainedSessions() > History::kMaxSessions)
			return 12;
	}
	// Descriptor pressure must never let Redo skip an undone initial boundary,
	// and the reclaimed range must stay bounded and allocation-free.
	for (SlotId s = 0; s < kTrackCount; ++s) {
		history.resetSlot(s);
		const auto live = pool.sealedHandle(s);
		if (live.valid())
			pool.retire(live);
	}
	rollback.service(200000);
	for (unsigned i = 0; i < 200000 && (history.retirementPending() || pool.retirementPending()); ++i) {
		history.serviceRetirement(1024);
		pool.serviceRetirement(1024);
	}
	if (history.retirementPending() || pool.retirementPending())
		return 21;
	auto record = [&](SlotId s, PhraseHandle& handle) -> bool {
		if (audio.beginInitial(s, 513) != Status::Ok)
			return false;
		audio.render(s, initial, output);
		return audio.finishInitial(s, handle) == Status::Ok && audio.start(s, false) == Status::Ok;
	};
	PhraseHandle base{}, keep[kTrackCount]{};
	if (!record(0, base) || !record(1, keep[1]) || !record(2, keep[2]))
		return 22;
	// Overdub slot 0, then Undo both layers to empty (retains initial + overdub).
	if (audio.setCaptureGate(0, true) != Status::Ok)
		return 23;
	audio.render(0, {&input, 1}, {output, 1});
	if (audio.setCaptureGate(0, false) != Status::Ok)
		return 24;
	if (drive(0, false) != Status::Ok)
		return 25;
	{
		const auto published = pool.sealedHandle(0);
		if (!published.valid() || audio.replace(0, published) != Status::Ok)
			return 26;
	}
	if (drive(0, false) != Status::Ok || pool.sealedHandle(0).valid() || audio.detachForHistory(0) != Status::Ok)
		return 27;
	history.mappingReplaced(0);
	if (!history.canRedo(0))
		return 28;
	// Fill every descriptor from another track only; never touch slot 0.
	auto overdubOnce = [&](SlotId s) -> Status {
		if (audio.setCaptureGate(s, true) != Status::Ok)
			return Status::Busy;
		audio.render(s, {&input, 1}, {output, 1});
		return audio.setCaptureGate(s, false);
	};
	while (history.retainedSessions() < History::kMaxSessions)
		if (overdubOnce(1) != Status::Ok)
			return 29;
	if (history.retainedSessions() != History::kMaxSessions)
		return 30;
	int32_t firstBefore[kTrackCount]{};
	for (SlotId s = 1; s <= 2; ++s)
		firstBefore[s] = pool.frame(pool.sealedHandle(s), 0)->l;
	// One more admission on another track evicts slot 0's undone initial base.
	history.beginSession(1, keep[1].generation);
	if (history.canRedo(0) || history.canUndo(0) || pool.sealedHandle(0).valid())
		return 31;
	RestoreId skipped;
	if (history.requestRestore(0, true, skipped) != Status::SourceUnavailable)
		return 32;
	for (SlotId s = 1; s <= 2; ++s)
		if (pool.frame(pool.sealedHandle(s), 0)->l != firstBefore[s])
			return 33;
	for (unsigned i = 0; i < 200000 && (history.retirementPending() || pool.retirementPending()); ++i) {
		history.serviceRetirement(1024);
		pool.serviceRetirement(1024);
	}
	if (history.retirementPending() || pool.retirementPending() || history.retainedSessions() > History::kMaxSessions)
		return 34;
	for (SlotId s = 1; s <= 2; ++s)
		if (pool.frame(pool.sealedHandle(s), 0)->l != firstBefore[s])
			return 35;
	return before == allocationCalls ? 0 : 13;
}
