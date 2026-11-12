#pragma once
#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/gesture_rollback.h"
#include "deluge/loop_station/history.h"
#include "deluge/loop_station/resident_audio.h"
#include "deluge/loop_station/snapshot.h"
#include <array>
#include <vector>
using namespace loop_station;
struct BeforeImageFixture {
	alignas(Frame) std::byte live[16 * kPageBytes]{};
	alignas(Frame) std::byte arena[4 * kPageBytes]{};
	PagePool pool;
	GestureRollback rollback{pool};
	History history{pool, rollback};
	SnapshotStore snapshots{pool, rollback};
	ResidentAudio audio{pool, rollback};
	std::vector<std::byte> mandatory;
	void setup() {
		CHECK(Status::Ok == pool.initialize(live));
		mandatory.resize(GestureRollback::requiredBytes());
		CHECK(Status::Ok == rollback.initialize(mandatory));
		audio.setWriteObservers(&history, &snapshots);
	}
	PhraseHandle phrase(SlotId slot = 0, unsigned length = 3) {
		std::vector<Frame> frames(length);
		for (unsigned i = 0; i < length; ++i)
			frames[i] = {int32_t(i + 1), -int32_t(i + 1)};
		CHECK(Status::Ok == pool.beginPhrase(slot, length));
		FrameIndex accepted{};
		CHECK(Status::Ok == pool.append(slot, frames, accepted));
		PhraseHandle handle;
		CHECK(Status::Ok == pool.seal(slot, handle));
		CHECK(Status::Ok == audio.attach(slot, handle));
		CHECK(Status::Ok == audio.start(slot, false));
		return handle;
	}
	void dub(SlotId slot = 0, unsigned count = 3) {
		CHECK(Status::Ok == audio.setCaptureGate(slot, true));
		const Frame input{3, -3};
		Frame output;
		for (unsigned i = 0; i < count; ++i)
			audio.render(slot, {&input, 1}, {&output, 1});
	}
	void serviceRetirement(uint32_t budget = 1024) {
		history.serviceRetirement(budget);
		pool.serviceRetirement(budget);
	}
};
