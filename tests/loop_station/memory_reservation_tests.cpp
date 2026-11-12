#include "../hardware/loop_station/fixture.h"
#include "CppUTest/TestHarness.h"
#include "before_image_fixture.h"
#include "deluge/loop_station/memory_reservation.h"
#include <cstdlib>
#include <map>
#include <vector>
using namespace loop_station;
namespace {
// Real host backing; only the firmware allocator boundary is substituted.
struct HostAllocator : ReservationAllocator {
	std::map<void*, size_t> live;
	std::vector<size_t> requests;
	unsigned failAt = 0, releases = 0;
	void* allocatePinnedLowSpeed(size_t bytes) override {
		requests.push_back(bytes);
		if (requests.size() == failAt)
			return nullptr;
		auto* result = std::malloc(bytes);
		if (result)
			live[result] = bytes;
		return result;
	}
	void deallocate(void* address) override {
		CHECK_EQUAL(1, live.erase(address));
		++releases;
		std::free(address);
	}
	~HostAllocator() override { CHECK(live.empty()); }
};
} // namespace
TEST_GROUP(LoopMemory){};
TEST(LoopMemory, exactProfilesHaveDisjointOwnedStorage) {
	HostAllocator allocator;
	MemoryReservation memory(allocator);
	for (auto profile : {MemoryProfile::Preferred, MemoryProfile::TenSecond}) {
		CHECK(Status::Ok == memory.acquire(profile));
		CHECK(profile == memory.profile());
		CHECK_EQUAL(profile == MemoryProfile::Preferred ? 33554432 : 28246016, memory.activeStorage().size());
		CHECK_EQUAL(4194304, memory.historyStorage().size());
		CHECK_EQUAL(4194304, memory.snapshotStorage().size());
		CHECK_EQUAL(GestureRollback::requiredBytes(), memory.gestureStorage().size());
		CHECK_EQUAL(4096, memory.stagingStorage().size());
		std::span<std::byte> spans[] = {memory.activeStorage(), memory.historyStorage(), memory.snapshotStorage(),
		                                memory.gestureStorage(), memory.stagingStorage()};
		for (unsigned a = 0; a < 5; ++a) {
			spans[a].front() = std::byte(a + 1);
			spans[a].back() = std::byte(a + 2);
			for (unsigned b = a + 1; b < 5; ++b) {
				auto first = reinterpret_cast<uintptr_t>(spans[a].data());
				auto second = reinterpret_cast<uintptr_t>(spans[b].data());
				CHECK(first + spans[a].size() <= second || second + spans[b].size() <= first);
			}
		}
		memory.release();
		CHECK(allocator.live.empty());
	}
	CHECK_EQUAL(10, allocator.releases);
}
TEST(LoopMemory, failedPreferredNeverFallsBackAndEachPartialAllocationIsReleasedOnce) {
	for (unsigned fail = 1; fail <= 5; ++fail) {
		HostAllocator allocator;
		allocator.failAt = fail;
		MemoryReservation memory(allocator);
		CHECK(Status::NoMemory == memory.acquire(MemoryProfile::Preferred));
		CHECK_EQUAL(fail, allocator.requests.size());
		CHECK_EQUAL(33554432, allocator.requests.front());
		CHECK_EQUAL(fail - 1, allocator.releases);
		CHECK(memory.activeStorage().empty());
		CHECK(allocator.live.empty());
		memory.release();
		CHECK_EQUAL(fail - 1, allocator.releases);
	}
}
TEST(LoopMemory, profileReplacementAndReleaseCannotInvalidateLiveConsumers) {
	HostAllocator allocator;
	MemoryReservation memory(allocator);
	CHECK(Status::Ok == memory.acquire(MemoryProfile::Preferred));
	auto* address = memory.activeStorage().data();
	{
		auto owner = memory.pin();
		CHECK(owner.valid());
		CHECK(Status::Busy == memory.acquire(MemoryProfile::TenSecond));
		CHECK_EQUAL(5, allocator.requests.size());
		memory.release();
		CHECK_EQUAL(0, allocator.releases);
		POINTERS_EQUAL(address, memory.activeStorage().data());
		CHECK_FALSE(memory.pin().valid()); // No new consumers while shutting down.
		address[0] = std::byte{42};
		CHECK_EQUAL(42, std::to_integer<unsigned>(memory.activeStorage()[0]));
	}
	CHECK_EQUAL(5, allocator.releases);
	CHECK(memory.activeStorage().empty());
	CHECK(Status::Ok == memory.acquire(MemoryProfile::TenSecond));
}
TEST(LoopMemory, repeatedEnableDisableAndInvalidRequestsHaveNoLeak) {
	HostAllocator allocator;
	MemoryReservation memory(allocator);
	CHECK_FALSE(memory.pin().valid());
	CHECK(Status::Invalid == memory.acquire(static_cast<MemoryProfile>(255)));
	CHECK(allocator.requests.empty());
	for (unsigned i = 0; i < 10; ++i) {
		CHECK(Status::Ok == memory.acquire(MemoryProfile::TenSecond));
		memory.release();
		memory.release();
		CHECK(allocator.live.empty());
	}
	CHECK_EQUAL(50, allocator.releases);
}

namespace {
uint32_t tickCounter() {
	static uint32_t tick = 0;
	tick += 23;
	return tick;
}
struct CountingSink : feasibility::ArchiveSink {
	unsigned writes = 0;
	Status write(std::span<const Frame> frames) override {
		CHECK(!frames.empty());
		++writes;
		return Status::Ok;
	}
};
} // namespace
TEST(LoopMemory, harnessRunsEightTracksTogglesCaptureAndDoesNotNeedArchiveProgress) {
	HostAllocator allocator;
	CountingSink sink;
	feasibility::Fixture fixture(allocator, tickCounter, &sink);
	CHECK(Status::Ok == fixture.start(MemoryProfile::TenSecond, 512));
	std::array<Frame, 128> input;
	input.fill({40, -80});
	for (unsigned i = 0; i < 4; ++i)
		fixture.callback(input);
	fixture.callback(input); // Seal initial phrases at exact boundary.
	auto state = fixture.diagnostics().snapshot();
	CHECK_EQUAL(255, state.playingMask);
	CHECK_EQUAL(0, state.captureMask);
	CHECK_EQUAL(5, state.callbacks);
	CHECK_EQUAL(115, state.callbackCycles);
	CHECK_EQUAL(23, state.maxCallbackCycles);
	CHECK_EQUAL(28246016, state.activeBytes);
	CHECK_EQUAL(4194304, state.snapshotBytes);
	CHECK_EQUAL(40343168, state.reservedBytes);
	for (unsigned slot = 0; slot < 8; ++slot)
		CHECK_EQUAL(512, state.capturedFrames[slot]);
	CHECK(Status::Ok == fixture.toggleCapture());
	fixture.callback(input);
	state = fixture.diagnostics().snapshot();
	CHECK_EQUAL(255, state.captureMask);
	CHECK_EQUAL(8, state.pageCopyPages);
	CHECK_EQUAL(4096, state.pageCopyFrames);
	// Bounded callback loop with archive explicitly disabled for multiple capture cycles.
	for (unsigned i = 0; i < 1100; ++i) {
		fixture.callback(input);
		fixture.serviceArchive();
	}
	CHECK_EQUAL(0, fixture.diagnostics().snapshot().storageCalls);
	CHECK_EQUAL(0, sink.writes);
	CHECK_EQUAL(255, fixture.diagnostics().snapshot().playingMask);
	fixture.setArchiveEnabled(true);
	fixture.serviceArchive();
	CHECK_EQUAL(8, fixture.diagnostics().snapshot().storageCalls);
	CHECK_EQUAL(8, sink.writes);
	fixture.stop();
	CHECK(allocator.live.empty());
	CHECK(Status::Ok == fixture.start(MemoryProfile::Preferred, 512));
	fixture.stop();
	CHECK(allocator.live.empty());
}
TEST(LoopMemory, harnessStartFailureUnwindsAndRejectsInvalidLength) {
	HostAllocator allocator;
	feasibility::Fixture fixture(allocator, tickCounter);
	CHECK(Status::Invalid == fixture.start(MemoryProfile::Preferred, 0));
	CHECK(allocator.requests.empty());
	allocator.failAt = 4;
	CHECK(Status::NoMemory == fixture.start(MemoryProfile::Preferred));
	CHECK(allocator.live.empty());
	CHECK_EQUAL(3, allocator.releases);
	CHECK(Status::Unavailable == fixture.toggleCapture());
	fixture.callback({});
	CHECK_EQUAL(0, fixture.diagnostics().snapshot().callbacks);
}
TEST(LoopMemory, diagnosticWorkCountsCopiesAndHistoryPressureAcrossArenaExhaustion) {
	BeforeImageFixture f;
	f.setup();
	CHECK(Status::Ok == f.history.initialize(std::span(f.arena).first(kPageBytes)));
	CHECK(Status::Ok == f.snapshots.initialize({}));
	f.phrase(0, 513);
	f.dub(0, 513);
	CHECK_EQUAL(1, f.history.work().copiedPages);
	CHECK_EQUAL(512, f.history.work().copiedFrames);
	CHECK_EQUAL(1, f.history.pressure());
	CHECK_EQUAL(1, f.history.retainedPages()); // Reclamation is deferred off the render path.
	f.serviceRetirement(16);
	CHECK_EQUAL(0, f.history.retainedPages());
	CHECK(Status::Ok == f.audio.setCaptureGate(0, false));
	f.dub(0, 1);
	CHECK_EQUAL(2, f.history.work().copiedPages); // Counter survives discarded history.
	CHECK_EQUAL(1024, f.history.work().copiedFrames);
}
TEST(LoopMemory, livePhraseSurvivesReleaseAndUnrelatedAllocationPressure) {
	HostAllocator allocator;
	MemoryReservation memory(allocator);
	CHECK(Status::Ok == memory.acquire(MemoryProfile::TenSecond));
	{
		auto pin = memory.pin();
		auto moved = std::move(pin);
		CHECK_FALSE(pin.valid());
		CHECK(moved.valid());
		PagePool pool;
		CHECK(Status::Ok == pool.initialize(memory.activeStorage()));
		CHECK(Status::Ok == pool.beginPhrase(0, 1));
		const Frame original{12345, -67890};
		FrameIndex accepted = 0;
		CHECK(Status::Ok == pool.append(0, {&original, 1}, accepted));
		PhraseHandle phrase;
		CHECK(Status::Ok == pool.seal(0, phrase));
		memory.release();
		for (unsigned i = 0; i < 20; ++i) {
			auto* other = allocator.allocatePinnedLowSpeed(1024 * 1024);
			CHECK(other != nullptr);
			std::fill_n(static_cast<std::byte*>(other), 1024 * 1024, std::byte{0x7f});
			CHECK_EQUAL(12345, pool.frame(phrase, 0)->l);
			CHECK_EQUAL(-67890, pool.frame(phrase, 0)->r);
			allocator.deallocate(other);
		}
		CHECK(Status::Ok == pool.release(phrase));
	}
	CHECK(allocator.live.empty());
}
TEST(LoopMemory, archiveYieldCanDisableHarnessWithoutFreeingStagingUnderWriter) {
	HostAllocator allocator;
	struct DisablingSink : feasibility::ArchiveSink {
		feasibility::Fixture* fixture = nullptr;
		HostAllocator* allocator = nullptr;
		Status write(std::span<const Frame> frames) override {
			const auto first = frames[0];
			fixture->stop();
			CHECK_EQUAL(5, allocator->live.size());
			CHECK_EQUAL(first.l, frames[0].l);
			CHECK_EQUAL(first.r, frames[0].r);
			return Status::Ok;
		}
	} sink;
	feasibility::Fixture fixture(allocator, tickCounter, &sink);
	sink.fixture = &fixture;
	sink.allocator = &allocator;
	CHECK(Status::Ok == fixture.start(MemoryProfile::TenSecond, 1));
	const Frame input{5, -6};
	fixture.callback({&input, 1});
	fixture.callback({&input, 1});
	fixture.setArchiveEnabled(true);
	fixture.serviceArchive();
	CHECK_EQUAL(1, fixture.diagnostics().snapshot().storageCalls);
	CHECK(allocator.live.empty());
}
TEST(LoopMemory, failedHarnessReportsRequestedProfileAndZeroAssignedBytes) {
	HostAllocator allocator;
	allocator.failAt = 1;
	feasibility::Fixture fixture(allocator, tickCounter);
	CHECK(Status::NoMemory == fixture.start(MemoryProfile::TenSecond));
	const auto diagnostic = fixture.diagnostics().snapshot();
	CHECK(MemoryProfile::TenSecond == diagnostic.profile);
	CHECK(Status::NoMemory == diagnostic.status);
	CHECK_EQUAL(0, diagnostic.reservedBytes);
	CHECK_EQUAL(1, allocator.requests.size());
	CHECK_EQUAL(28246016, allocator.requests.front());
}

// A missed crossing loses a before-image; copying on every write exceeds this
// two-page-per-observer bound even though every individual copy is bounded.
TEST(LoopMemory, eightSimultaneousPageCrossingsBoundBothObserversAndPreserveSnapshots) {
	BeforeImageFixture f;
	f.setup();
	std::vector<std::byte> history(16 * kPageBytes), snapshots(16 * kPageBytes);
	CHECK(Status::Ok == f.history.initialize(history));
	CHECK(Status::Ok == f.snapshots.initialize(snapshots));
	SnapshotId ids[8];
	PhraseHandle handles[8];
	std::array<Frame, 511> silence{}, output;
	for (SlotId slot = 0; slot < 8; ++slot) {
		handles[slot] = f.phrase(slot, 1024);
		f.audio.render(slot, silence, output);
		CHECK_EQUAL(511, f.audio.position(slot));
		CHECK(Status::Ok == f.snapshots.request(slot, handles[slot].generation, ids[slot]));
		CHECK(Status::Ok == f.rollback.begin(slot, 511, 511, 1024));
		CHECK(Status::Ok == f.audio.setCaptureGate(slot, true));
	}
	std::array<Frame, 128> input, crossed;
	input.fill({3, -3});
	for (SlotId slot = 0; slot < 8; ++slot) {
		const auto before = f.rollback.work();
		f.audio.render(slot, input, crossed);
		// 128 ordinary writes at <=1024 steps each, plus four page copies
		// reading 512 frames at <=16 AVL lookup steps per frame.
		CHECK(f.rollback.work().treeSteps - before.treeSteps <= 163840);
		CHECK(f.rollback.work().capturedFrames - before.capturedFrames <= 128);
		CHECK_EQUAL(512, crossed[0].l);
		CHECK_EQUAL(513, crossed[1].l);
		CHECK_EQUAL(515, f.pool.frame(handles[slot], 511)->l);
		CHECK_EQUAL(-516, f.pool.frame(handles[slot], 512)->r);
	}
	CHECK_EQUAL(16, f.history.work().copiedPages);
	CHECK_EQUAL(16, f.snapshots.work().copiedPages);
	CHECK_EQUAL(8192, f.history.work().copiedFrames);
	CHECK_EQUAL(8192, f.snapshots.work().copiedFrames);
	for (SlotId slot = 0; slot < 8; ++slot) {
		Frame original[2];
		CHECK(Status::Ok == f.snapshots.read(ids[slot], 511, original));
		CHECK_EQUAL(512, original[0].l);
		CHECK_EQUAL(-513, original[1].r);
		f.audio.render(slot, input, crossed);
		CHECK_EQUAL(640, crossed[0].l);
	}
	CHECK_EQUAL(16, f.history.work().copiedPages);
	CHECK_EQUAL(16, f.snapshots.work().copiedPages);
	CHECK_EQUAL(0, f.pool.freePages());
}

// History exhaustion must neither replace mandatory protection nor make work
// grow with the number of saturated wraps.
TEST(LoopMemory, historyDisabledEightSaturatedOneFrameLoopsKeepConstantWriteAndCancelWork) {
	BeforeImageFixture f;
	f.setup();
	CHECK(Status::Ok == f.history.initialize({}));
	CHECK(Status::Ok == f.snapshots.initialize({}));
	PhraseHandle handles[8];
	for (SlotId slot = 0; slot < 8; ++slot) {
		handles[slot] = f.phrase(slot, 1);
		CHECK(Status::Ok == f.rollback.begin(slot, 0, 0, 1));
		CHECK(Status::Ok == f.audio.setCaptureGate(slot, true));
	}
	const Frame input{INT32_MAX, INT32_MIN};
	Frame output;
	for (unsigned frame = 0; frame < 15434; ++frame) {
		for (SlotId slot = 0; slot < 8; ++slot) {
			const auto before = f.rollback.work();
			f.audio.render(slot, {&input, 1}, {&output, 1});
			CHECK_EQUAL(frame == 0 ? 1 : INT32_MAX, output.l);
			CHECK_EQUAL(frame == 0 ? -1 : INT32_MIN, output.r);
			CHECK(f.rollback.work().treeSteps - before.treeSteps <= 2);
			CHECK_EQUAL(0, f.rollback.work().retiredFrames - before.retiredFrames);
		}
	}
	CHECK_EQUAL(8, f.rollback.work().capturedFrames);
	CHECK_EQUAL(0, f.history.work().copiedPages);
	CHECK_EQUAL(0, f.snapshots.work().copiedPages);
	for (SlotId slot = 0; slot < 8; ++slot) {
		const auto before = f.rollback.work();
		CHECK(Status::Ok == f.rollback.cancel(slot));
		CHECK_EQUAL(before.treeSteps, f.rollback.work().treeSteps);
		CHECK(Status::Ok == f.audio.setCaptureGate(slot, false));
		f.audio.render(slot, {&input, 1}, {&output, 1});
		CHECK_EQUAL(1, output.l);
		CHECK_EQUAL(-1, output.r);
		CHECK_FALSE(f.history.canUndo(slot));
	}
	for (unsigned credit = 0; credit < 8; ++credit) {
		const auto before = f.rollback.work();
		f.rollback.service(1);
		CHECK_EQUAL(1, f.rollback.work().retiredFrames - before.retiredFrames);
		CHECK(f.rollback.work().treeSteps - before.treeSteps <= 256);
	}
	CHECK_EQUAL(8, f.rollback.work().restoredFrames);
	for (SlotId slot = 0; slot < 8; ++slot) {
		CHECK_FALSE(f.rollback.pending(slot));
		CHECK_EQUAL(1, f.pool.frame(handles[slot], 0)->l);
		CHECK_EQUAL(-1, f.pool.frame(handles[slot], 0)->r);
	}
}
