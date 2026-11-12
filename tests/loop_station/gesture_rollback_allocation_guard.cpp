#include "deluge/loop_station/gesture_rollback.h"
#include "deluge/loop_station/resident_audio.h"

#include <cstdlib>
#include <new>

static unsigned allocationCalls = 0;
void* operator new(std::size_t size) {
	++allocationCalls;
	if (void* p = std::malloc(size))
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
	alignas(Frame) static std::byte backing[8 * 32 * kPageBytes]{};
	alignas(Frame) static std::byte scratch[GestureRollback::requiredBytes()]{};
	PagePool pool;
	GestureRollback rollback(pool);
	ResidentAudio audio(pool, rollback);
	if (pool.initialize(backing) != Status::Ok)
		return 1;
	const auto before = allocationCalls;
	if (rollback.initialize(scratch) != Status::Ok)
		return 2;
	static Frame initial[16000]{};
	const Frame input[1] = {{INT32_MAX, INT32_MIN}};
	Frame output[1];
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		if (audio.beginInitial(slot, 16000) != Status::Ok)
			return 3;
		static Frame initialOutput[16000];
		audio.render(slot, initial, initialOutput);
		PhraseHandle handle;
		if (audio.finishInitial(slot, handle) != Status::Ok || audio.start(slot, false) != Status::Ok
		    || rollback.begin(slot, 0, 0, 16000) != Status::Ok || audio.setCaptureGate(slot, true) != Status::Ok)
			return 4;
		for (unsigned i = 0; i < 15434; ++i) {
			const auto work = rollback.work();
			audio.render(slot, input, output);
			if (rollback.work().treeSteps - work.treeSteps > 1024)
				return 5;
		}
		if (rollback.cancel(slot) != Status::Ok)
			return 6;
		for (unsigned i = 0; i < 17000; ++i) {
			if (rollback.begin(slot, 15434 + i, audio.position(slot), 16000) != Status::Ok)
				return 7;
			const auto work = rollback.work();
			audio.render(slot, input, output);
			if (rollback.work().treeSteps - work.treeSteps > 1024)
				return 8;
			if ((i & 1 ? rollback.confirm(slot) : rollback.cancel(slot)) != Status::Ok)
				return 9;
		}
	}
	rollback.advance(1000000);
	for (unsigned i = 0; i < 8 * 15435; ++i) {
		const auto work = rollback.work();
		rollback.service(1);
		if (rollback.work().treeSteps - work.treeSteps > 256)
			return 10;
	}
	return allocationCalls == before ? 0 : 11;
}
