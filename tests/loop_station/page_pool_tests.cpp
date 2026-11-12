#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/page_pool.h"

#include <array>
#include <vector>

using namespace loop_station;

TEST_GROUP(LoopPages){};

TEST(LoopPages, manualSlotsSharePagesOneRoundAtATime) {
	std::vector<std::byte> storage(16 * kPageBytes);
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	for (SlotId slot = 0; slot < 8; ++slot)
		CHECK(Status::Ok == pool.beginPhrase(slot, 0));
	std::array<Frame, 1024> input{};
	CHECK(Status::Ok == pool.beginRenderRound(0xff));
	for (SlotId slot = 0; slot < 8; ++slot) {
		input.fill(Frame{slot + 1, slot + 101});
		FrameIndex accepted = 0;
		CHECK(Status::Ok == pool.append(slot, input, accepted));
		CHECK_EQUAL(512, accepted);
	}
	CHECK_EQUAL(8, pool.freePages());
	CHECK(Status::Ok == pool.beginRenderRound(0xff));
	for (SlotId slot = 0; slot < 8; ++slot) {
		input.fill(Frame{slot + 1, slot + 101});
		FrameIndex accepted = 0;
		CHECK(Status::Ok == pool.append(slot, input, accepted));
		CHECK_EQUAL(512, accepted);
	}
	CHECK_EQUAL(0, pool.freePages());
	for (SlotId slot = 0; slot < 8; ++slot) {
		PhraseHandle handle;
		CHECK(Status::Ok == pool.seal(slot, handle));
		CHECK(handle.valid());
		CHECK_EQUAL(slot + 1, pool.frame(handle, 0)->l);
		CHECK_EQUAL(slot + 101, pool.frame(handle, 1023)->r);
		POINTERS_EQUAL(nullptr, pool.frame(handle, 1024));
		CHECK(Status::Ok == pool.release(handle));
		POINTERS_EQUAL(nullptr, pool.frame(handle, 0));
		CHECK(Status::Stale == pool.release(handle));
	}
	CHECK_EQUAL(16, pool.freePages());
}

TEST(LoopPages, interleavedStartsShareOneDeclaredRenderRound) {
	std::vector<std::byte> storage(4 * kPageBytes);
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	CHECK(Status::Ok == pool.beginRenderRound(0x03));
	CHECK(Status::Ok == pool.beginPhrase(0, 0));
	std::array<Frame, 1024> input{};
	input.fill({11, 12});
	FrameIndex accepted = 0;
	CHECK(Status::Ok == pool.append(0, input, accepted));
	CHECK_EQUAL(512, accepted);
	CHECK(Status::Ok == pool.beginPhrase(1, 0));
	input.fill({21, 22});
	CHECK(Status::Ok == pool.append(1, input, accepted));
	CHECK_EQUAL(512, accepted);
	CHECK_EQUAL(2, pool.freePages());
	CHECK(Status::Ok == pool.beginRenderRound(0x03));
	CHECK(Status::Ok == pool.append(0, input, accepted));
	CHECK_EQUAL(512, accepted);
	CHECK(Status::Ok == pool.append(1, input, accepted));
	CHECK_EQUAL(512, accepted);
	CHECK_EQUAL(0, pool.freePages());
}

TEST(LoopPages, repeatedAppendCannotClaimAnotherPageWithinSameRenderRound) {
	std::vector<std::byte> storage(4 * kPageBytes);
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	CHECK(Status::Ok == pool.beginRenderRound(0x03));
	CHECK(Status::Ok == pool.beginPhrase(0, 0));
	CHECK(Status::Ok == pool.beginPhrase(1, 0));
	std::array<Frame, 512> input{};
	FrameIndex accepted = 0;
	CHECK(Status::Ok == pool.append(0, input, accepted));
	CHECK_EQUAL(512, accepted);
	CHECK(Status::Ok == pool.append(0, input, accepted));
	CHECK_EQUAL(0, accepted);
	CHECK_EQUAL(2, pool.freePages());
	CHECK(Status::Ok == pool.append(1, input, accepted));
	CHECK_EQUAL(512, accepted);
	CHECK(Status::Ok == pool.append(0, input, accepted));
	CHECK_EQUAL(0, accepted);
	CHECK_EQUAL(2, pool.freePages());
}

TEST(LoopPages, undersizedRoundGrantsDoNotDependOnAppendOrder) {
	for (bool reverse : {false, true}) {
		std::vector<std::byte> storage(kPageBytes);
		PagePool pool;
		CHECK(Status::Ok == pool.initialize(storage));
		CHECK(Status::Ok == pool.beginPhrase(0, 0));
		CHECK(Status::Ok == pool.beginPhrase(1, 0));
		CHECK(Status::Ok == pool.beginRenderRound(0x03));
		CHECK_EQUAL(0, pool.freePages());
		const Frame input[512]{};
		FrameIndex accepted = 99;
		const SlotId first = reverse ? 1 : 0;
		const SlotId second = reverse ? 0 : 1;
		const auto firstStatus = pool.append(first, input, accepted);
		CHECK(firstStatus == (first == 0 ? Status::Ok : Status::Busy));
		CHECK_EQUAL(first == 0 ? 512 : 0, accepted);
		const auto secondStatus = pool.append(second, input, accepted);
		CHECK(secondStatus == (second == 0 ? Status::Ok : Status::Busy));
		CHECK_EQUAL(second == 0 ? 512 : 0, accepted);
		CHECK_EQUAL(0, pool.freePages());
	}
}

TEST(LoopPages, remainderGrantRotatesWhenSpaceReturnsInLaterRender) {
	std::vector<std::byte> storage(2 * kPageBytes);
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	CHECK(Status::Ok == pool.beginPhrase(2, 1));
	CHECK(Status::Ok == pool.beginPhrase(0, 0));
	CHECK(Status::Ok == pool.beginPhrase(1, 0));
	CHECK(Status::Ok == pool.beginRenderRound(0x03));
	const Frame input[512]{};
	FrameIndex accepted = 99;
	CHECK(Status::Busy == pool.append(1, input, accepted));
	CHECK_EQUAL(0, accepted);
	CHECK(Status::Ok == pool.append(0, input, accepted));
	CHECK_EQUAL(512, accepted);
	CHECK(Status::Ok == pool.beginRenderRound(0x03));
	CHECK(Status::NoMemory == pool.append(1, input, accepted));
	CHECK_EQUAL(0, accepted);
	PhraseHandle empty;
	CHECK(Status::Invalid == pool.seal(2, empty));
	CHECK(Status::Ok == pool.beginRenderRound(0x03));
	CHECK(Status::Busy == pool.append(0, input, accepted));
	CHECK_EQUAL(0, accepted);
	CHECK(Status::Ok == pool.append(1, input, accepted));
	CHECK_EQUAL(512, accepted);
	CHECK_EQUAL(0, pool.freePages());
}

TEST(LoopPages, manualAppendRequiresDeclaredRoundParticipation) {
	std::vector<std::byte> storage(2 * kPageBytes);
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	CHECK(Status::Ok == pool.beginPhrase(0, 0));
	CHECK(Status::Ok == pool.beginPhrase(1, 0));
	const Frame input[] = {{1, 2}};
	FrameIndex accepted = 99;
	CHECK(Status::Busy == pool.append(0, input, accepted));
	CHECK_EQUAL(0, accepted);
	CHECK(Status::Ok == pool.beginRenderRound(0x02));
	CHECK(Status::Busy == pool.append(0, input, accepted));
	CHECK_EQUAL(0, accepted);
	CHECK(Status::Ok == pool.append(1, input, accepted));
	CHECK_EQUAL(1, accepted);
	CHECK_EQUAL(1, pool.freePages());
}

TEST(LoopPages, fixedReservationFailsWithoutClaimingPages) {
	std::vector<std::byte> storage(3 * kPageBytes);
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	CHECK(Status::NoMemory == pool.beginPhrase(0, 1537));
	CHECK_EQUAL(3, pool.freePages());
	CHECK(Status::Ok == pool.beginPhrase(0, 1025));
	CHECK_EQUAL(0, pool.freePages());
	CHECK(Status::NoMemory == pool.beginPhrase(1, 1));
	CHECK_EQUAL(0, pool.freePages());
}

TEST(LoopPages, rejectedAdmissionPreservesExistingPhraseAndReusedSlotInvalidatesOldHandle) {
	std::vector<std::byte> storage(kPageBytes);
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	CHECK(Status::Ok == pool.beginPhrase(0, 512));
	const Frame input[] = {{123, -456}};
	FrameIndex accepted = 0;
	CHECK(Status::Ok == pool.append(0, input, accepted));
	CHECK_EQUAL(1, accepted);
	PhraseHandle first;
	CHECK(Status::Ok == pool.seal(0, first));
	CHECK(Status::NoMemory == pool.beginPhrase(1, 1));
	CHECK_EQUAL(123, pool.frame(first, 0)->l);
	CHECK_EQUAL(-456, pool.frame(first, 0)->r);
	CHECK(Status::Ok == pool.release(first));
	CHECK(Status::Ok == pool.beginPhrase(0, 1));
	accepted = 0;
	CHECK(Status::Ok == pool.append(0, input, accepted));
	PhraseHandle second;
	CHECK(Status::Ok == pool.seal(0, second));
	CHECK(second.generation != first.generation);
	POINTERS_EQUAL(nullptr, pool.frame(first, 0));
	CHECK(Status::Stale == pool.release(first));
}

TEST(LoopPages, zeroFrameSealIsInvalidAndDoesNotLeakPages) {
	std::vector<std::byte> storage(kPageBytes);
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	CHECK(Status::Ok == pool.beginPhrase(0, 0));
	PhraseHandle handle;
	CHECK(Status::Invalid == pool.seal(0, handle));
	CHECK_FALSE(handle.valid());
	CHECK_EQUAL(1, pool.freePages());
}

TEST(LoopPages, bothProfilesFitEightIndependentTenSecondTakes) {
	for (const auto profile : {MemoryProfile::Preferred, MemoryProfile::TenSecond}) {
		std::vector<std::byte> storage(activeAudioBytes(profile));
		PagePool pool;
		CHECK(Status::Ok == pool.initialize(storage));
		for (SlotId slot = 0; slot < 8; ++slot)
			CHECK(Status::Ok == pool.beginPhrase(slot, 441000));
		CHECK_EQUAL(profile == MemoryProfile::TenSecond ? 0 : 1296, pool.freePages());
	}
	std::vector<std::byte> reduced(activeAudioBytes(MemoryProfile::TenSecond));
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(reduced));
	for (SlotId slot = 0; slot < 6; ++slot)
		CHECK(Status::Ok == pool.beginPhrase(slot, 529200));
	const auto remaining = pool.freePages();
	CHECK(Status::NoMemory == pool.beginPhrase(6, 529200));
	CHECK_EQUAL(remaining, pool.freePages());
}
