#include "before_image_fixture.h"
TEST_GROUP(LoopSnapshots), BeforeImageFixture{void setup() override{BeforeImageFixture::setup();
}
}
;
TEST(LoopSnapshots, remainsCoherentWhileLivePhraseBecomesFourFiveSix) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	auto handle = phrase();
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, handle.generation, id));
	dub();
	Frame staging[3];
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	for (int i = 0; i < 3; ++i) {
		CHECK_EQUAL(i + 1, staging[i].l);
		CHECK_EQUAL(i + 4, pool.frame(handle, i)->l);
	}
	CHECK(snapshots.valid(id));
	snapshots.release(id);
	CHECK_FALSE(snapshots.valid(id));
	CHECK(Status::Stale == snapshots.read(id, 0, staging));
}
TEST(LoopSnapshots, pagePressureInvalidatesBlockedWriterBeforeMixedDataCanPublish) {
	CHECK(Status::Ok == snapshots.initialize(std::span(arena).first(kPageBytes)));
	auto handle = phrase(0, 513);
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, handle.generation, id));
	Frame staging[1];
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	// Simulate yielding SD with only fixed staging retained. Audio continues.
	dub(0, 513);
	CHECK_FALSE(snapshots.valid(id));
	CHECK(Status::Stale == snapshots.read(id, 512, staging));
	CHECK_EQUAL(4, pool.frame(handle, 0)->l);
	CHECK_EQUAL(516, pool.frame(handle, 512)->l);
	SnapshotId replacement;
	CHECK(Status::Ok == snapshots.request(0, handle.generation, replacement));
	CHECK(replacement.generation != id.generation);
	snapshots.release(id);
	CHECK(snapshots.valid(replacement));
}
TEST(LoopSnapshots, requestCoalescesAndGenerationsRejectCanceledDescriptorReuse) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	auto handle = phrase();
	SnapshotId old;
	for (unsigned i = 0; i < 100000; ++i) {
		SnapshotId id, same;
		CHECK(Status::Ok == snapshots.request(0, handle.generation, id));
		CHECK(Status::Ok == snapshots.request(0, handle.generation, same));
		CHECK_EQUAL(id.generation, same.generation);
		CHECK_FALSE(snapshots.valid(old));
		CHECK(Status::Ok == snapshots.cancel(id));
		CHECK_FALSE(snapshots.valid(id));
		Frame staging;
		CHECK(Status::Stale == snapshots.read(id, 0, {&staging, 1}));
		old = id;
	}
	CHECK_EQUAL(0, snapshots.retainedPages());
	CHECK_EQUAL(0, snapshots.activeSnapshots());
}
TEST(LoopSnapshots, canceledGestureInvalidatesEvenUntouchedLivePages) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	auto handle = phrase(0, 513);
	CHECK(Status::Ok == rollback.begin(0, 0, 0, 513));
	dub(0, 1);
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, handle.generation, id));
	Frame staging[1];
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	CHECK_EQUAL(4, staging[0].l);
	CHECK(Status::Ok == rollback.cancel(0));
	CHECK_FALSE(snapshots.valid(id));
	CHECK(Status::Stale == snapshots.read(id, 512, staging));
}
TEST(LoopSnapshots, snapshotRequestedDuringCanceledRetirementReadsEffectiveOriginals) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	auto handle = phrase();
	CHECK(Status::Ok == rollback.begin(0, 0, 0, 3));
	dub();
	CHECK(Status::Ok == rollback.cancel(0));
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, handle.generation, id));
	dub();
	rollback.service(100);
	Frame staging[3];
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	for (int i = 0; i < 3; ++i)
		CHECK_EQUAL(i + 1, staging[i].l);
}
TEST(LoopSnapshots, emptyArenaCancelsBeforeFirstWriteWithoutInterruptingCapture) {
	CHECK(Status::Ok == snapshots.initialize({}));
	auto handle = phrase();
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, handle.generation, id));
	dub(0, 6);
	CHECK_FALSE(snapshots.valid(id));
	CHECK_EQUAL(7, pool.frame(handle, 0)->l);
}
TEST(LoopSnapshots, allSlotsPressureAndClearRejectStalePinGenerations) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	SnapshotId ids[kTrackCount];
	PhraseHandle handles[kTrackCount];
	for (SlotId s = 0; s < kTrackCount; ++s) {
		handles[s] = phrase(s);
		CHECK(Status::Ok == snapshots.request(s, handles[s].generation, ids[s]));
	}
	CHECK_EQUAL(kTrackCount, snapshots.activeSnapshots());
	for (SlotId s = 0; s < kTrackCount; ++s)
		dub(s);
	CHECK(snapshots.retainedPages() <= 4);
	for (SlotId s = 0; s < kTrackCount; ++s) {
		Frame staging[3];
		const auto status = snapshots.read(ids[s], 0, staging);
		if (status == Status::Ok)
			for (int i = 0; i < 3; ++i)
				CHECK_EQUAL(i + 1, staging[i].l);
		else
			CHECK(Status::Stale == status);
		audio.stop(s);
		CHECK(Status::Ok == audio.clear(s));
		CHECK_FALSE(snapshots.valid(ids[s]));
		phrase(s);
		CHECK_FALSE(snapshots.valid(ids[s]));
	}
}
TEST(LoopSnapshots, invalidRequestsAndBoundedStagingNeverReturnPartialSuccess) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	auto handle = phrase();
	SnapshotId id;
	CHECK(Status::Invalid == snapshots.request(kInvalidSlot, 1, id));
	CHECK(Status::Stale == snapshots.request(0, handle.generation + 1, id));
	CHECK(Status::Ok == snapshots.request(0, handle.generation, id));
	Frame staging[513];
	staging[0] = {99, 99};
	CHECK(Status::Invalid == snapshots.read(id, 0, staging));
	CHECK_EQUAL(99, staging[0].l);
	CHECK(Status::Invalid == snapshots.read(id, 3, std::span(staging).first(1)));
	CHECK(Status::Stale == snapshots.cancel({}));
	CHECK(Status::Ok == snapshots.cancel(id));
	CHECK(Status::Stale == snapshots.cancel(id));
}
TEST(LoopSnapshots, crossPageStagingAndConfirmKeepTheSameSnapshotGeneration) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	auto handle = phrase(0, 513);
	CHECK(Status::Ok == rollback.begin(0, 0, 0, 513));
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, handle.generation, id));
	dub(0, 513);
	CHECK(Status::Ok == rollback.confirm(0));
	rollback.service(1000);
	Frame staging[2];
	CHECK(Status::Ok == snapshots.read(id, 511, staging));
	CHECK_EQUAL(512, staging[0].l);
	CHECK_EQUAL(513, staging[1].l);
	CHECK(snapshots.valid(id));
}
TEST(LoopSnapshots, subpageCapacityAndMisalignedArenaAreExplicitAndRepeatedInitializeIsBusy) {
	CHECK(Status::Invalid == snapshots.initialize(std::span(arena).subspan(1)));
	CHECK(Status::Ok == snapshots.initialize(std::span(arena).first(kPageBytes - 1)));
	CHECK(Status::Busy == snapshots.initialize(arena));
	auto handle = phrase();
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, handle.generation, id));
	dub();
	CHECK_FALSE(snapshots.valid(id));
}
