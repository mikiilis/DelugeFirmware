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
	return before == allocationCalls ? 0 : 13;
}
