#include "before_image_fixture.h"

namespace {
Status beginReserved(PagePool& pool, SlotId slot, FrameIndex frames, PhraseHandle& handle) {
	const auto begun = pool.beginDetached(slot, frames, handle);
	if (begun != Status::Ok)
		return begun;
	for (unsigned attempt = 0; attempt < PagePool::kMaxPagesPerRecord / 8 + 1; ++attempt) {
		const auto reserved = pool.reserveDetachedPages(handle, 8);
		if (reserved != Status::Busy)
			return reserved;
	}
	return Status::Busy;
}

Status reserveRestore(History& history, RestoreId id) {
	for (unsigned attempt = 0; attempt < PagePool::kMaxPagesPerRecord / 8 + 1; ++attempt) {
		const auto reserved = history.reserveRestore(id, 8);
		if (reserved != Status::Busy)
			return reserved;
	}
	return Status::Busy;
}
} // namespace

TEST_GROUP(LoopRestore), BeforeImageFixture{void setup() override{BeforeImageFixture::setup();
}
}
;

TEST(LoopRestore, detachedCandidateCanBePreparedWithoutTouchingTheLivePhrase) {
	auto live = phrase(0, 5);
	PhraseHandle candidate{};
	CHECK(Status::Ok == beginReserved(pool, 0, 513, candidate));
	for (FrameIndex i = 0; i < 513; ++i)
		CHECK(Status::Ok == pool.writeDetached(candidate, i, {int32_t(1000 + i), -int32_t(1000 + i)}));
	CHECK(Status::Ok == pool.sealDetached(candidate));
	CHECK_EQUAL(5, pool.length(live));
	CHECK_EQUAL(1, pool.frame(live, 0)->l);
	CHECK_EQUAL(5, pool.frame(live, 4)->l);
	CHECK_EQUAL(513, pool.length(candidate));
	CHECK_EQUAL(1000, pool.frame(candidate, 0)->l);
	CHECK_EQUAL(1512, pool.frame(candidate, 512)->l);
}

TEST(LoopRestore, detachedCandidateExhaustionLeavesLivePhraseAndHandleValid) {
	auto live = phrase(0, 5);
	// The fixture owns sixteen pages. The live phrase and a full sixteen-page
	// candidate cannot coexist, so reservation must fail before exposing a handle.
	PhraseHandle candidate{};
	CHECK(Status::Ok == pool.beginDetached(0, 16 * (kPageBytes / sizeof(Frame)), candidate));
	CHECK(Status::NoMemory == pool.reserveDetachedPages(candidate, 64));
	CHECK(Status::Ok == pool.retire(candidate));
	pool.serviceRetirement(16);
	CHECK_EQUAL(5, pool.length(live));
	CHECK_EQUAL(3, pool.frame(live, 2)->l);
}

TEST(LoopRestore, competingPhrasePressureFailsReservationPromptlyWithoutTouchingLivePhrase) {
	auto live = phrase(0, 5);
	constexpr FrameIndex framesPerPage = kPageBytes / sizeof(Frame);
	constexpr FrameIndex candidateFrames = 6 * framesPerPage;
	CHECK(pool.canBeginDetached(0, candidateFrames));
	PhraseHandle candidate{};
	CHECK(Status::Ok == pool.beginDetached(0, candidateFrames, candidate));
	CHECK(Status::Ok == pool.beginPhrase(1, 10 * framesPerPage)); // Leaves only five pages for a six-page candidate.
	CHECK(Status::NoMemory == pool.reserveDetachedPages(candidate, 8));
	CHECK_EQUAL(5, pool.length(live));
	CHECK_EQUAL(1, pool.frame(live, 0)->l);
	CHECK(Status::Ok == pool.retire(candidate));
	pool.serviceRetirement(16);
}

TEST(LoopRestore, releasedCurrentRecordCannotAliasAnotherTracksDetachedPhrase) {
	phrase(0, 5);
	PhraseHandle detached{};
	CHECK(Status::Ok == beginReserved(pool, 0, 2, detached));
	CHECK(Status::Ok == pool.writeDetached(detached, 0, {20, -20}));
	CHECK(Status::Ok == pool.writeDetached(detached, 1, {21, -21}));
	CHECK(Status::Ok == pool.sealDetached(detached));
	PhraseHandle previous{};
	CHECK(Status::Ok == pool.replace(detached, previous));
	CHECK(Status::Ok == pool.release(previous));
	CHECK(Status::Ok == pool.release(pool.sealedHandle(0)));
	CHECK_FALSE(pool.sealedHandle(0).valid());
	CHECK(Status::Ok == pool.beginPhrase(0, 2));
	Frame frames[2]{{30, -30}, {31, -31}};
	FrameIndex accepted = 0;
	CHECK(Status::Ok == pool.append(0, frames, accepted));
	CHECK_EQUAL(2, accepted);
	PhraseHandle replacement{};
	CHECK(Status::Ok == pool.seal(0, replacement));
	CHECK_EQUAL(30, pool.frame(pool.sealedHandle(0), 0)->l);
}

TEST(LoopRestore, incompleteChangedSessionRemainsAsUndoFenceAfterBeforeImagesRetire) {
	CHECK(Status::Ok == history.initialize(std::span(arena).first(kPageBytes)));
	phrase(0, 513);
	dub(0, 1);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	dub(0, 513); // The next unique page cannot be retained in the one-page arena.
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK_FALSE(history.canUndo(0));
	CHECK_EQUAL(1, history.retainedPages());
	history.serviceRetirement(16);
	CHECK_EQUAL(0, history.retainedPages());
	CHECK_FALSE(history.canUndo(0)); // The incomplete newest boundary still fences older undo.
}

TEST(LoopRestore, resetDuringActiveOverdubDetachesHistoryAndReclaimsInForeground) {
	CHECK(Status::Ok == history.initialize(arena));
	phrase();
	dub(0, 1);
	CHECK_EQUAL(1, history.retainedSessions());
	history.resetSlot(0);
	CHECK_FALSE(history.canUndo(0));
	CHECK_EQUAL(1, history.retainedSessions());
	for (unsigned i = 0; i < 80 && history.retainedSessions(); ++i) {
		history.serviceRetirement(16);
		pool.serviceRetirement(16);
	}
	CHECK_EQUAL(0, history.retainedSessions());
}

TEST(LoopRestore, failedPhraseAdmissionDoesNotReserveARecordForAnotherTracksBuilder) {
	PhraseHandle live[kTrackCount]{};
	PhraseHandle current[kTrackCount]{};
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		live[slot] = phrase(slot, 1);
		CHECK(Status::Ok == beginReserved(pool, slot, 1, current[slot]));
		CHECK(Status::Ok == pool.writeDetached(current[slot], 0, {int32_t(10 + slot), -int32_t(10 + slot)}));
		CHECK(Status::Ok == pool.sealDetached(current[slot]));
		PhraseHandle previous{};
		CHECK(Status::Ok == pool.replace(current[slot], previous));
		CHECK_EQUAL(live[slot].generation, previous.generation);
	}
	CHECK(Status::Ok == pool.release(current[0]));       // All original record rows stay retained.
	CHECK(Status::NoMemory == pool.beginPhrase(0, 513)); // Needs two pages; only one is free.
	PhraseHandle otherBuilder{};
	CHECK(Status::Ok == beginReserved(pool, 1, 1, otherBuilder));
	CHECK(Status::NoMemory == pool.beginPhrase(0, 1)); // No stale record alias may report Busy.
	CHECK(Status::Ok == pool.release(otherBuilder));
	CHECK(Status::Ok == pool.beginPhrase(0, 1));
	Frame one{99, -99};
	FrameIndex accepted{};
	CHECK(Status::Ok == pool.append(0, {&one, 1}, accepted));
	PhraseHandle next{};
	CHECK(Status::Ok == pool.seal(0, next));
	CHECK_EQUAL(99, pool.frame(next, 0)->l);
}

TEST(LoopRestore, replacementSwapsOwnershipAndKeepsOldVersionUntilExplicitRetirement) {
	auto live = phrase(0, 5);
	PhraseHandle candidate{};
	CHECK(Status::Ok == beginReserved(pool, 0, 3, candidate));
	CHECK(Status::Ok == pool.writeDetached(candidate, 0, {70, -70}));
	CHECK(Status::Ok == pool.writeDetached(candidate, 1, {80, -80}));
	CHECK(Status::Ok == pool.writeDetached(candidate, 2, {90, -90}));
	CHECK(Status::Ok == pool.sealDetached(candidate));
	PhraseHandle previous{};
	CHECK(Status::Ok == pool.replace(candidate, previous));
	CHECK_EQUAL(live.generation, previous.generation);
	CHECK_EQUAL(candidate.generation, pool.sealedHandle(0).generation);
	CHECK_EQUAL(3, pool.length(pool.sealedHandle(0)));
	CHECK_EQUAL(80, pool.frame(pool.sealedHandle(0), 1)->l);
	CHECK_EQUAL(5, pool.length(previous));
	CHECK_EQUAL(3, pool.frame(previous, 2)->l);
	CHECK(Status::Ok == pool.release(previous));
	CHECK_EQUAL(0, pool.length(previous));
	CHECK_EQUAL(3, pool.length(pool.sealedHandle(0)));
}

TEST(LoopRestore, logicalRetirementIsImmediateAndPageReclaimIsBounded) {
	auto live = phrase(0, 513);
	const auto freeBefore = pool.freePages();
	PhraseHandle candidate{};
	CHECK(Status::Ok == beginReserved(pool, 0, 2, candidate));
	CHECK(Status::Ok == pool.writeDetached(candidate, 0, {70, -70}));
	CHECK(Status::Ok == pool.writeDetached(candidate, 1, {80, -80}));
	CHECK(Status::Ok == pool.sealDetached(candidate));
	PhraseHandle previous{};
	CHECK(Status::Ok == pool.replace(candidate, previous));
	CHECK(Status::Ok == pool.retire(previous));
	CHECK_EQUAL(0, pool.length(previous));
	CHECK_EQUAL(freeBefore - 1, pool.freePages());
	CHECK_EQUAL(1, pool.serviceRetirement(1));
	CHECK_EQUAL(freeBefore, pool.freePages());
	CHECK(pool.retirementPending());
	CHECK(Status::Ok == pool.beginPhrase(1, 1)); // The retiring descriptor cannot be reused early.
	Frame otherTrack{900, -900};
	FrameIndex accepted = 0;
	CHECK(Status::Ok == pool.append(1, {&otherTrack, 1}, accepted));
	CHECK_EQUAL(1, accepted);
	PhraseHandle other{};
	CHECK(Status::Ok == pool.seal(1, other));
	CHECK_EQUAL(1, pool.serviceRetirement(1));
	CHECK_FALSE(pool.retirementPending());
	CHECK_EQUAL(freeBefore, pool.freePages());
	CHECK_EQUAL(900, pool.frame(other, 0)->l);
	CHECK_EQUAL(2, pool.length(pool.sealedHandle(0)));
	CHECK_EQUAL(70, pool.frame(pool.sealedHandle(0), 0)->l);
	CHECK_EQUAL(0, pool.length(live)); // Old generation is never visible after retirement begins.
}

TEST(LoopRestore, audioRebindPreservesRunningPhaseModuloRestoredLength) {
	auto live = phrase(0, 5);
	const Frame input{};
	Frame output{};
	for (unsigned i = 0; i < 4; ++i)
		audio.render(0, {&input, 1}, {&output, 1});
	PhraseHandle candidate{};
	CHECK(Status::Ok == beginReserved(pool, 0, 3, candidate));
	for (FrameIndex i = 0; i < 3; ++i)
		CHECK(Status::Ok == pool.writeDetached(candidate, i, {int32_t(50 + i), -int32_t(50 + i)}));
	CHECK(Status::Ok == pool.sealDetached(candidate));
	PhraseHandle previous{};
	CHECK(Status::Ok == pool.replace(candidate, previous));
	CHECK(Status::Ok == audio.replace(0, candidate));
	CHECK_EQUAL(1, audio.position(0)); // old phase 4 modulo restored length 3
	audio.render(0, {&input, 1}, {&output, 1});
	CHECK_EQUAL(51, output.l);
	CHECK_EQUAL(2, audio.position(0));
	CHECK_EQUAL(5, pool.length(live));
}

TEST(LoopRestore, stoppedReplacementStartsAtZeroAndOneShotKeepsItsCompletionMode) {
	auto live = phrase(0, 5);
	CHECK(Status::Ok == audio.start(0, true));
	const Frame input{};
	Frame output{};
	for (unsigned i = 0; i < 4; ++i)
		audio.render(0, {&input, 1}, {&output, 1});
	PhraseHandle candidate{};
	CHECK(Status::Ok == beginReserved(pool, 0, 3, candidate));
	for (FrameIndex i = 0; i < 3; ++i)
		CHECK(Status::Ok == pool.writeDetached(candidate, i, {int32_t(60 + i), -int32_t(60 + i)}));
	CHECK(Status::Ok == pool.sealDetached(candidate));
	PhraseHandle previous{};
	CHECK(Status::Ok == pool.replace(candidate, previous));
	CHECK(Status::Ok == audio.replace(0, candidate));
	CHECK_EQUAL(1, audio.position(0));
	const Frame two[2]{};
	Frame outputTwo[2]{};
	audio.render(0, two, outputTwo);
	CHECK_EQUAL(61, outputTwo[0].l);
	CHECK_EQUAL(62, outputTwo[1].l);
	CHECK(CompletionReason::OneShot == audio.takeCompletion(0).reason);
	CHECK_EQUAL(0, audio.position(0));
	audio.stop(0);
	PhraseHandle stoppedCandidate{};
	CHECK(Status::Ok == beginReserved(pool, 0, 2, stoppedCandidate));
	CHECK(Status::Ok == pool.writeDetached(stoppedCandidate, 0, {90, -90}));
	CHECK(Status::Ok == pool.writeDetached(stoppedCandidate, 1, {91, -91}));
	CHECK(Status::Ok == pool.sealDetached(stoppedCandidate));
	CHECK(Status::Ok == pool.replace(stoppedCandidate, previous));
	CHECK(Status::Ok == audio.replace(0, stoppedCandidate));
	CHECK_EQUAL(0, audio.position(0));
	CHECK_EQUAL(5, pool.length(live));
}

TEST(LoopRestore, cancellationReleasesOnlyTheDetachedCandidate) {
	CHECK(Status::Ok == history.initialize(arena));
	auto live = phrase(0, 513);
	dub(0, 1);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	const auto freeBefore = pool.freePages();
	const auto firstFrame = pool.frame(live, 0)->l;
	RestoreId restore{};
	CHECK(Status::Ok == history.requestRestore(0, false, restore));
	CHECK_EQUAL(freeBefore, pool.freePages()); // Request publishes metadata only; page reservation is service work.
	CHECK(Status::Ok == reserveRestore(history, restore));
	CHECK(Status::Busy == history.serviceRestore(restore, 10));
	CHECK_EQUAL(freeBefore - 2, pool.freePages());
	history.cancelRestore(0);
	CHECK(Status::Stale == history.serviceRestore(restore, 10));
	CHECK_EQUAL(freeBefore - 2, pool.freePages());
	CHECK_EQUAL(2, pool.serviceRetirement(2));
	CHECK_EQUAL(freeBefore, pool.freePages());
	CHECK_EQUAL(firstFrame, pool.frame(live, 0)->l);
	CHECK_EQUAL(513, pool.length(live));
}

TEST(LoopRestore, newEditCancelsPendingRestoreAndInvalidatesItsRevision) {
	CHECK(Status::Ok == history.initialize(arena));
	auto live = phrase(0, 513);
	dub(0, 1);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	RestoreId restore{};
	CHECK(Status::Ok == history.requestRestore(0, false, restore));
	CHECK(Status::Ok == reserveRestore(history, restore));
	CHECK(Status::Busy == history.serviceRestore(restore, 4));
	CHECK(Status::Ok == audio.setCaptureGate(0, true)); // Edit start cancels the builder.
	const Frame input{2, -2};
	Frame output{};
	audio.render(0, {&input, 1}, {&output, 1});
	CHECK(Status::Stale == history.serviceRestore(restore, 8));
	CHECK_EQUAL(4, pool.frame(live, 0)->l);
	CHECK_EQUAL(4, pool.frame(live, 1)->l);
}

TEST(LoopRestore, clearThenNewTakeThenDubUndoesOnlyTheNewPhrase) {
	CHECK(Status::Ok == history.initialize(arena));
	phrase(0, 3);
	dub(0, 3);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	audio.stop(0);
	CHECK(Status::Ok == audio.clear(0));
	CHECK_FALSE(history.canUndo(0));
	auto replacement = phrase(0, 3);
	dub(0, 3);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	RestoreId restore{};
	CHECK(Status::Ok == history.requestRestore(0, false, restore));
	CHECK(Status::Ok == reserveRestore(history, restore));
	Status status = Status::Busy;
	while (status == Status::Busy)
		status = history.serviceRestore(restore, 3);
	CHECK(Status::Ok == status);
	CHECK(Status::Ok == history.commitRestore(restore));
	CHECK_EQUAL(1, pool.frame(pool.sealedHandle(0), 0)->l);
	CHECK_EQUAL(3, pool.frame(pool.sealedHandle(0), 2)->l);
	CHECK_EQUAL(3, pool.length(replacement));
}

TEST(LoopRestore, multiPageUndoAndRedoRestoreExactFrames) {
	CHECK(Status::Ok == history.initialize(arena));
	auto live = phrase(0, 513);
	dub(0, 513);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	CHECK_EQUAL(516, pool.frame(live, 512)->l);
	RestoreId restore{};
	CHECK(Status::Ok == history.requestRestore(0, false, restore));
	CHECK(Status::Ok == reserveRestore(history, restore));
	Status progress = Status::Busy;
	while (progress == Status::Busy)
		progress = history.serviceRestore(restore, 73);
	CHECK(Status::Ok == progress);
	CHECK(Status::Ok == history.commitRestore(restore));
	auto restored = pool.sealedHandle(0);
	CHECK_EQUAL(513, pool.length(restored));
	CHECK_EQUAL(1, pool.frame(restored, 0)->l);
	CHECK_EQUAL(513, pool.frame(restored, 512)->l);
	CHECK(Status::Ok == audio.replace(0, restored));
	CHECK(history.canRedo(0));
	CHECK(Status::Ok == history.requestRestore(0, true, restore));
	CHECK(Status::Ok == reserveRestore(history, restore));
	CHECK(Status::Ok == history.serviceRestore(restore, 1));
	CHECK(Status::Ok == history.commitRestore(restore));
	CHECK_EQUAL(516, pool.frame(pool.sealedHandle(0), 512)->l);
	CHECK(history.canUndo(0));
}

TEST(LoopRestore, cursorTraversesMultipleSessionsInChronologicalOrder) {
	CHECK(Status::Ok == history.initialize(arena));
	phrase(0, 3);
	auto edit = [&] {
		CHECK(Status::Ok == audio.setCaptureGate(0, true));
		const Frame input[3]{{3, -3}, {3, -3}, {3, -3}};
		Frame output[3]{};
		audio.render(0, input, output);
		CHECK(Status::Ok == audio.setCaptureGate(0, false));
	};
	edit(); // Initial state [1,2,3] -> [4,2,3]
	edit(); // [4,2,3] -> [7,2,3]
	auto restore = [&](bool redo) {
		RestoreId id{};
		CHECK(Status::Ok == history.requestRestore(0, redo, id));
		CHECK(Status::Ok == reserveRestore(history, id));
		Status result = Status::Busy;
		while (result == Status::Busy)
			result = history.serviceRestore(id, 2);
		CHECK(Status::Ok == result);
		CHECK(Status::Ok == history.commitRestore(id));
		CHECK(Status::Ok == audio.replace(0, pool.sealedHandle(0)));
	};
	CHECK_EQUAL(7, pool.frame(pool.sealedHandle(0), 0)->l);
	CHECK(history.canUndo(0));
	restore(false);
	CHECK_EQUAL(4, pool.frame(pool.sealedHandle(0), 0)->l);
	CHECK(history.canUndo(0));
	CHECK(history.canRedo(0));
	restore(false);
	CHECK_EQUAL(1, pool.frame(pool.sealedHandle(0), 0)->l);
	CHECK_FALSE(history.canUndo(0));
	CHECK(history.canRedo(0));
	restore(true);
	CHECK_EQUAL(4, pool.frame(pool.sealedHandle(0), 0)->l);
	restore(true);
	CHECK_EQUAL(7, pool.frame(pool.sealedHandle(0), 0)->l);
}

TEST(LoopRestore, incompleteNewestSessionBlocksSkippingToOlderUndo) {
	CHECK(Status::Ok == history.initialize(std::span(arena).first(kPageBytes)));
	auto handle = phrase(0, 513);
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	const Frame input{3, -3};
	Frame output{};
	audio.render(0, {&input, 1}, {&output, 1});
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	dub(0, 513);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK_FALSE(history.canUndo(0));
	RestoreId id{};
	CHECK(Status::SourceUnavailable == history.requestRestore(0, false, id));
	CHECK_EQUAL(7, pool.frame(handle, 0)->l);
}

TEST(LoopRestore, undoCandidateExhaustionReturnsNoMemoryWithoutChangingThePhrase) {
	CHECK(Status::Ok == history.initialize(arena));
	auto live = phrase(0, 16 * (kPageBytes / sizeof(Frame)));
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	const Frame input{7, -7};
	Frame output{};
	audio.render(0, {&input, 1}, {&output, 1});
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK_FALSE(history.canUndo(0));
	RestoreId restore{};
	CHECK(Status::NoMemory == history.requestRestore(0, false, restore));
	CHECK_EQUAL(8, pool.frame(live, 0)->l);
	CHECK_EQUAL(0, pool.freePages());
}
