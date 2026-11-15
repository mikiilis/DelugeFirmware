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
	// A base recording is built through the real initial-take path, so it also
	// registers an initial history boundary and leaves the phrase playing.
	PhraseHandle phrase(SlotId slot = 0, unsigned length = 3) {
		std::vector<Frame> frames(length);
		for (unsigned i = 0; i < length; ++i)
			frames[i] = {int32_t(i + 1), -int32_t(i + 1)};
		std::vector<Frame> captured(length);
		CHECK(Status::Ok == audio.beginInitial(slot, length));
		audio.render(slot, frames, captured);
		PhraseHandle handle;
		CHECK(Status::Ok == audio.finishInitial(slot, handle));
		CHECK(Status::Ok == audio.start(slot, false));
		return handle;
	}
	// Drive one confirmed History step to completion at the current boundary and
	// publish it through ResidentAudio. Returns the published handle, or an
	// invalid handle when the step emptied the slot (initial Undo).
	PhraseHandle restoreStep(SlotId slot, bool redo) {
		RestoreId id{};
		CHECK(Status::Ok == history.requestRestore(slot, redo, id));
		for (unsigned attempt = 0; attempt < PagePool::kMaxPagesPerRecord / 8 + 1; ++attempt) {
			const auto reserved = history.reserveRestore(id, 8);
			if (reserved != Status::Busy) {
				CHECK(Status::Ok == reserved);
				break;
			}
		}
		Status status = Status::Busy;
		while (status == Status::Busy)
			status = history.serviceRestore(id, 8);
		CHECK(Status::Ok == status);
		CHECK(Status::Ok == history.commitRestore(id));
		const auto published = pool.sealedHandle(slot);
		if (published.valid()) {
			CHECK(Status::Ok == audio.replace(slot, published));
			return published;
		}
		CHECK(Status::Ok == audio.detachForHistory(slot));
		history.mappingReplaced(slot);
		return {};
	}
	void expectFrames(SlotId slot, std::initializer_list<int32_t> values) {
		const auto handle = pool.sealedHandle(slot);
		CHECK(handle.valid());
		unsigned index = 0;
		for (const auto value : values) {
			CHECK(pool.frame(handle, index) != nullptr);
			CHECK_EQUAL(value, pool.frame(handle, index)->l);
			CHECK_EQUAL(-value, pool.frame(handle, index)->r);
			++index;
		}
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
