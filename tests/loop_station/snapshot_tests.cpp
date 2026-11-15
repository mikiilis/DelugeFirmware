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

// A snapshot acquired before a confirmed navigation is frozen and owned by the
// store: initial Undo detaches the provider to empty and Redo republishes it,
// yet the acquired view still reads its exact original frames throughout.
TEST(LoopSnapshots, RetainedSnapshotReadsOriginalFramesAcrossInitialUndoAndRedo) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	alignas(Frame) std::byte historyBytes[8 * kPageBytes]{};
	CHECK(Status::Ok == history.initialize(historyBytes));
	const auto base = phrase(0, 5);
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, base.generation, id));
	CHECK(Status::Ok == snapshots.preserve(0));
	CHECK(snapshots.valid(id));

	const auto emptied = restoreStep(0, false);
	CHECK_FALSE(emptied.valid());
	CHECK_FALSE(pool.sealedHandle(0).valid());
	CHECK(snapshots.valid(id));
	Frame staging[5]{};
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	for (int i = 0; i < 5; ++i) {
		CHECK_EQUAL(i + 1, staging[i].l);
		CHECK_EQUAL(-(i + 1), staging[i].r);
	}

	const auto redone = restoreStep(0, true);
	CHECK(redone.valid());
	CHECK(snapshots.valid(id));
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	for (int i = 0; i < 5; ++i) {
		CHECK_EQUAL(i + 1, staging[i].l);
		CHECK_EQUAL(-(i + 1), staging[i].r);
	}
	snapshots.release(id);
	CHECK_FALSE(snapshots.valid(id));
	CHECK_EQUAL(0, snapshots.activeSnapshots());
}

// An ordinary overdub Undo retains the original record as the Redo version; the
// frozen snapshot keeps reading the overdub's original frames before and after
// Redo, not the overdubbed or later-written frames.
TEST(LoopSnapshots, RetainedSnapshotKeepsOriginalFramesAcrossOverdubUndoAndRedo) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	alignas(Frame) std::byte historyBytes[8 * kPageBytes]{};
	CHECK(Status::Ok == history.initialize(historyBytes));
	const auto base = phrase(0, 5);
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, base.generation, id));
	CHECK(Status::Ok == snapshots.preserve(0));
	dub(0, 5); // Adds {3,-3} per frame and captures the snapshot's before-images.
	CHECK(Status::Ok == audio.setCaptureGate(0, false));

	const auto undone = restoreStep(0, false);
	CHECK(undone.valid());
	CHECK(snapshots.valid(id));
	Frame staging[5]{};
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	for (int i = 0; i < 5; ++i) {
		CHECK_EQUAL(i + 1, staging[i].l);
		CHECK_EQUAL(-(i + 1), staging[i].r);
	}

	const auto redone = restoreStep(0, true);
	CHECK(redone.valid());
	CHECK(snapshots.valid(id));
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	for (int i = 0; i < 5; ++i) {
		CHECK_EQUAL(i + 1, staging[i].l);
		CHECK_EQUAL(-(i + 1), staging[i].r);
	}
	snapshots.release(id);
}

// Retirement cannot reclaim a preserved version's pages until the snapshot is
// released; the release then drops the pin and bounded retirement reclaims it.
TEST(LoopSnapshots, RetainedSnapshotSurvivesRetirementUntilReleaseThenReclaims) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	const auto base = phrase(0, 5);
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, base.generation, id));
	CHECK(Status::Ok == snapshots.preserve(0));
	const auto freeBefore = pool.freePages();
	CHECK(Status::Ok == pool.retire(base));
	for (unsigned i = 0; i < 64; ++i)
		pool.serviceRetirement(64);
	CHECK(pool.retirementPending());
	CHECK_EQUAL(freeBefore, pool.freePages());
	CHECK(snapshots.valid(id));
	Frame staging[5]{};
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	for (int i = 0; i < 5; ++i)
		CHECK_EQUAL(i + 1, staging[i].l);

	snapshots.release(id);
	for (unsigned i = 0; i < 64; ++i)
		pool.serviceRetirement(64);
	CHECK_FALSE(pool.retirementPending());
	CHECK_EQUAL(freeBefore + 1, pool.freePages());
	CHECK_EQUAL(0, snapshots.activeSnapshots());
}

// A newer same-slot recording must not be folded into a preserved snapshot: its
// writes target a different version, so the snapshot keeps its original frames.
TEST(LoopSnapshots, RetainedSnapshotIgnoresANewerSameSlotRecordingWrite) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	const auto original = phrase(0, 5);
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, original.generation, id));
	CHECK(Status::Ok == snapshots.preserve(0));

	PhraseHandle retained{};
	CHECK(Status::Ok == pool.detach(0, retained));
	const Frame other[] = {{70, -70}, {80, -80}, {90, -90}};
	PhraseHandle replacement{};
	CHECK(Status::Ok == pool.beginDetached(0, 3, replacement));
	CHECK(Status::Ok == pool.reserveDetachedPages(replacement, 16));
	for (FrameIndex i = 0; i < 3; ++i)
		CHECK(Status::Ok == pool.writeDetached(replacement, i, other[i]));
	CHECK(Status::Ok == pool.sealDetached(replacement));
	PhraseHandle previous{};
	CHECK(Status::Ok == pool.replace(replacement, previous));
	CHECK_FALSE(previous.valid());
	CHECK_EQUAL(replacement.generation, pool.sealedHandle(0).generation);

	Frame staging[5]{};
	snapshots.beforeWrite(0, 0, {999, -999});
	CHECK(snapshots.valid(id));
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	CHECK_EQUAL(1, staging[0].l);
	CHECK_EQUAL(-1, staging[0].r);
	CHECK_EQUAL(5, staging[4].l);
	CHECK_EQUAL(-5, staging[4].r);
	snapshots.release(id);
	CHECK(Status::Ok == pool.release(original));
}

// Explicit Clear stays destructive for a preserved snapshot: it drops the pin and
// the retained pages, and the version is no longer readable.
TEST(LoopSnapshots, PreservedSnapshotIsInvalidatedByExplicitClear) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	const auto base = phrase(0, 5);
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, base.generation, id));
	CHECK(Status::Ok == snapshots.preserve(0));
	CHECK(snapshots.valid(id));
	audio.stop(0);
	CHECK(Status::Ok == audio.clear(0));
	CHECK_FALSE(snapshots.valid(id));
	CHECK_EQUAL(0, snapshots.retainedPages());
	CHECK_EQUAL(0, snapshots.activeSnapshots());
	for (unsigned i = 0; i < 64; ++i)
		pool.serviceRetirement(64);
	CHECK_FALSE(pool.retirementPending());
}

// A retired-but-pinned source is still readable and must never be republished or
// aliased: replace rejects it, a new take claims a fresh generation, and the
// preserved view keeps its original frames until release/drain reclaims it.
TEST(LoopSnapshots, RetiredPinnedSourceIsNotAPublicationCandidate) {
	CHECK(Status::Ok == snapshots.initialize(arena));
	const auto base = phrase(0, 5);
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, base.generation, id));
	CHECK(Status::Ok == snapshots.preserve(0));
	CHECK(Status::Ok == pool.retire(base));
	CHECK(snapshots.valid(id));
	Frame staging[5]{};
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	CHECK_EQUAL(1, staging[0].l);

	// Publication of the obsolete handle is rejected; the active mapping is empty.
	PhraseHandle previous{};
	CHECK(Status::Stale == pool.replace(base, previous));
	CHECK_FALSE(previous.valid());
	CHECK_FALSE(pool.sealedHandle(0).valid());

	// A new take claims a different generation and must not alias the pinned one.
	const Frame input[] = {{7, -7}, {8, -8}, {9, -9}};
	PhraseHandle fresh{};
	CHECK(Status::Ok == pool.beginPhrase(0, 3));
	FrameIndex accepted = 0;
	CHECK(Status::Ok == pool.append(0, input, accepted));
	CHECK(Status::Ok == pool.seal(0, fresh));
	CHECK(fresh.generation != base.generation);
	CHECK(snapshots.valid(id));
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	for (int i = 0; i < 5; ++i) {
		CHECK_EQUAL(i + 1, staging[i].l);
		CHECK_EQUAL(-(i + 1), staging[i].r);
	}

	snapshots.release(id);
	CHECK(Status::Ok == pool.release(fresh));
	for (unsigned i = 0; i < 64; ++i)
		pool.serviceRetirement(64);
	CHECK_FALSE(pool.retirementPending());
	CHECK_EQUAL(0, snapshots.activeSnapshots());
	CHECK_EQUAL(16, pool.freePages());
}
