#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/page_pool.h"

#include <array>
#include <vector>

using namespace loop_station;

namespace {
PhraseHandle sealReservedPhrase(PagePool& pool, SlotId slot, std::span<const Frame> frames) {
	CHECK(Status::Ok == pool.beginPhrase(slot, static_cast<FrameIndex>(frames.size())));
	FrameIndex accepted = 0;
	CHECK(Status::Ok == pool.append(slot, frames, accepted));
	CHECK_EQUAL(static_cast<FrameIndex>(frames.size()), accepted);
	PhraseHandle handle;
	CHECK(Status::Ok == pool.seal(slot, handle));
	return handle;
}

PhraseHandle sealDetachedPhrase(PagePool& pool, SlotId slot, std::span<const Frame> frames) {
	PhraseHandle handle;
	CHECK(Status::Ok == pool.beginDetached(slot, static_cast<FrameIndex>(frames.size()), handle));
	CHECK(Status::Ok == pool.reserveDetachedPages(handle, PagePool::kMaxPagesPerRecord));
	for (FrameIndex index = 0; index < frames.size(); ++index)
		CHECK(Status::Ok == pool.writeDetached(handle, index, frames[index]));
	CHECK(Status::Ok == pool.sealDetached(handle));
	return handle;
}
} // namespace

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
	PhraseHandle empty{0, 45678};
	CHECK(Status::Invalid == pool.seal(2, empty));
	CHECK_FALSE(empty.valid());
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

TEST(LoopPages, detachRetainsFramesWithoutFreeingPages) {
	alignas(Frame) std::byte storage[16 * kPageBytes]{};
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	std::vector<Frame> frames(5);
	for (unsigned i = 0; i < frames.size(); ++i)
		frames[i] = {int32_t(i + 1), -int32_t(i + 1)};
	PhraseHandle live[kTrackCount]{};
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		live[slot] = sealReservedPhrase(pool, slot, frames);
	const auto freeBefore = pool.freePages();
	CHECK_EQUAL(8, freeBefore);
	PhraseHandle retained[kTrackCount]{};
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		CHECK(Status::Ok == pool.detach(slot, retained[slot]));
		CHECK_EQUAL(live[slot].generation, retained[slot].generation);
		CHECK_FALSE(pool.sealedHandle(slot).valid());
		CHECK_EQUAL(5, pool.length(retained[slot]));
		CHECK_EQUAL(3, pool.frame(retained[slot], 2)->l);
		CHECK_EQUAL(-3, pool.frame(retained[slot], 2)->r);
		CHECK_EQUAL(freeBefore, pool.freePages());
		for (SlotId other = 0; other < kTrackCount; ++other) {
			if (other == slot)
				continue;
			if (other > slot) {
				CHECK_EQUAL(live[other].generation, pool.sealedHandle(other).generation);
				CHECK_EQUAL(1, pool.frame(pool.sealedHandle(other), 0)->l);
				CHECK_EQUAL(5, pool.frame(pool.sealedHandle(other), 4)->l);
			}
			else {
				CHECK_EQUAL(live[other].generation, retained[other].generation);
				CHECK_EQUAL(1, pool.frame(retained[other], 0)->l);
				CHECK_EQUAL(5, pool.length(retained[other]));
			}
		}
	}
	CHECK_EQUAL(freeBefore, pool.freePages());
}

TEST(LoopPages, detachFailureLeavesActiveMappingIntact) {
	alignas(Frame) std::byte storage[4 * kPageBytes]{};
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	PhraseHandle retained{};
	// A rejected detach must reset the caller's output, not leave a seeded handle.
	retained = {0, 12345};
	CHECK(Status::Invalid == pool.detach(kTrackCount, retained));
	CHECK_FALSE(retained.valid());
	retained = {0, 12345};
	CHECK(Status::Invalid == pool.detach(1, retained));
	CHECK_FALSE(retained.valid());
	CHECK_EQUAL(4, pool.freePages());
	const Frame input[] = {{11, -11}, {12, -12}};
	CHECK(Status::Ok == pool.beginPhrase(0, 2));
	FrameIndex accepted = 0;
	CHECK(Status::Ok == pool.append(0, input, accepted));
	CHECK_EQUAL(2, accepted);
	retained = {0, 12345};
	CHECK(Status::Invalid == pool.detach(0, retained));
	CHECK_FALSE(retained.valid());
	CHECK_FALSE(pool.sealedHandle(0).valid());
	PhraseHandle sealed{};
	CHECK(Status::Ok == pool.seal(0, sealed));
	CHECK(sealed.valid());
	CHECK_EQUAL(11, pool.frame(sealed, 0)->l);
	CHECK_EQUAL(-12, pool.frame(sealed, 1)->r);
	CHECK_EQUAL(3, pool.freePages());
}

TEST(LoopPages, replaceIntoDetachedEmptyTrackPublishesOnlyCandidate) {
	alignas(Frame) std::byte storage[4 * kPageBytes]{};
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	std::vector<Frame> liveFrames(5);
	for (unsigned i = 0; i < liveFrames.size(); ++i)
		liveFrames[i] = {int32_t(i + 1), -int32_t(i + 1)};
	const auto live = sealReservedPhrase(pool, 0, liveFrames);
	const auto freeBefore = pool.freePages();
	PhraseHandle retained{};
	CHECK(Status::Ok == pool.detach(0, retained));
	CHECK_EQUAL(freeBefore, pool.freePages());
	const Frame candidateFrames[] = {{70, -70}, {80, -80}, {90, -90}};
	const auto candidate = sealDetachedPhrase(pool, 0, candidateFrames);
	PhraseHandle previous{};
	CHECK(Status::Ok == pool.replace(candidate, previous));
	CHECK_FALSE(previous.valid());
	CHECK_EQUAL(candidate.generation, pool.sealedHandle(0).generation);
	CHECK_EQUAL(3, pool.length(pool.sealedHandle(0)));
	CHECK_EQUAL(80, pool.frame(pool.sealedHandle(0), 1)->l);
	CHECK_EQUAL(live.generation, retained.generation);
	CHECK_EQUAL(5, pool.length(retained));
	CHECK_EQUAL(3, pool.frame(retained, 2)->l);
	CHECK_EQUAL(freeBefore - 1, pool.freePages());
	PhraseHandle rejected{};
	const PhraseHandle stale{0, retained.generation + 1000000};
	rejected = {0, 23456};
	CHECK(Status::Stale == pool.replace(stale, rejected));
	CHECK_FALSE(rejected.valid());
	const auto otherTrack = sealDetachedPhrase(pool, 1, candidateFrames);
	const PhraseHandle foreign{0, otherTrack.generation};
	rejected = {0, 23456};
	CHECK(Status::Stale == pool.replace(foreign, rejected));
	CHECK_FALSE(rejected.valid());
	CHECK_EQUAL(candidate.generation, pool.sealedHandle(0).generation);
}

TEST(LoopPages, pinnedRecordDefersRetirementUntilUnpinnedThenReclaimsBoundedly) {
	alignas(Frame) std::byte storage[4 * kPageBytes]{};
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	std::vector<Frame> frames(5);
	for (unsigned i = 0; i < frames.size(); ++i)
		frames[i] = {int32_t(i + 1), -int32_t(i + 1)};
	const auto live = sealReservedPhrase(pool, 0, frames);
	const auto freeBefore = pool.freePages();
	CHECK(Status::Ok == pool.pin(live));
	CHECK(Status::Ok == pool.retire(live));
	for (unsigned i = 0; i < 16; ++i)
		pool.serviceRetirement(16);
	// The pinned version stays readable; retirement is deferred, not discarded.
	CHECK(pool.retirementPending());
	CHECK_EQUAL(freeBefore, pool.freePages());
	CHECK_EQUAL(5, pool.length(live));
	CHECK_EQUAL(3, pool.frame(live, 2)->l);
	CHECK_EQUAL(-5, pool.frame(live, 4)->r);
	CHECK(Status::Busy == pool.release(live)); // A snapshot still owns this version.
	// Releasing the last pin lets bounded retirement reclaim exactly those pages.
	CHECK(Status::Ok == pool.unpin(live));
	for (unsigned i = 0; i < 16; ++i)
		pool.serviceRetirement(16);
	CHECK_FALSE(pool.retirementPending());
	CHECK_EQUAL(freeBefore + 1, pool.freePages());
	CHECK_EQUAL(0, pool.length(live));
	CHECK(Status::Stale == pool.unpin(live));
	PhraseHandle invalid{};
	CHECK(Status::Stale == pool.pin(invalid));
	CHECK(Status::Invalid == pool.unpin(invalid));
}

// A retiring pinned version stays readable through the read-through lookup but is
// never a publication or reuse candidate: replace must not install it, and a new
// phrase must not alias its generation. Release/drain then reclaims it.
TEST(LoopPages, retiredRecordIsNotAPublicationCandidate) {
	alignas(Frame) std::byte storage[4 * kPageBytes]{};
	PagePool pool;
	CHECK(Status::Ok == pool.initialize(storage));
	std::vector<Frame> frames(5);
	for (unsigned i = 0; i < frames.size(); ++i)
		frames[i] = {int32_t(i + 1), -int32_t(i + 1)};
	const auto live = sealReservedPhrase(pool, 0, frames);
	CHECK(Status::Ok == pool.pin(live));
	CHECK(Status::Ok == pool.retire(live));

	// Rejected as a publication candidate; no mapping change and the output reset.
	PhraseHandle previous{0, 34567};
	CHECK(Status::Stale == pool.replace(live, previous));
	CHECK_FALSE(previous.valid());
	CHECK_FALSE(pool.sealedHandle(0).valid());
	// Still readable while pinned.
	CHECK_EQUAL(5, pool.length(live));
	CHECK_EQUAL(3, pool.frame(live, 2)->l);
	CHECK_EQUAL(-5, pool.frame(live, 4)->r);

	// A fresh take claims a different generation; the pinned one is untouched.
	PhraseHandle fresh{};
	CHECK(Status::Ok == pool.beginPhrase(0, 3));
	const Frame input[] = {{7, -7}, {8, -8}, {9, -9}};
	FrameIndex accepted = 0;
	CHECK(Status::Ok == pool.append(0, input, accepted));
	CHECK_EQUAL(3, accepted);
	CHECK(Status::Ok == pool.seal(0, fresh));
	CHECK(fresh.generation != live.generation);
	CHECK_EQUAL(5, pool.length(live));
	CHECK_EQUAL(3, pool.frame(live, 2)->l);
	CHECK_EQUAL(9, pool.frame(fresh, 2)->l);

	// Release/drain reclaims every expected page with no permanent retirement.
	CHECK(Status::Ok == pool.unpin(live));
	CHECK(Status::Ok == pool.release(fresh));
	for (unsigned i = 0; i < 16; ++i)
		pool.serviceRetirement(16);
	CHECK_FALSE(pool.retirementPending());
	CHECK_EQUAL(4, pool.freePages());
	CHECK_EQUAL(0, pool.length(live));
	CHECK_EQUAL(0, pool.length(fresh));
}
