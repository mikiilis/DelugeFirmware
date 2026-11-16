#include "before_image_fixture.h"
TEST_GROUP(LoopHistory), BeforeImageFixture{void setup() override{BeforeImageFixture::setup();
}
}
;
TEST(LoopHistory, retainsFirstPageImageAcrossRepeatedWrapsAndCoalescesOpenGate) {
	CHECK(Status::Ok == history.initialize(arena));
	phrase();
	dub(0, 9);
	CHECK_EQUAL(1, history.retainedPages());
	CHECK_FALSE(history.canUndo(0));
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	CHECK_EQUAL(2, history.retainedSessions()); // Initial boundary plus the overdub.
}
TEST(LoopHistory, emptyOrTinyArenaNeverClosesCaptureAndMandatoryCancelStillRestores) {
	CHECK(Status::Ok == history.initialize({}));
	auto handle = phrase();
	CHECK(Status::Ok == rollback.begin(0, 0, 0, 3));
	dub(0, 9);
	CHECK_EQUAL(10, pool.frame(handle, 0)->l);
	CHECK_FALSE(history.canUndo(0));
	CHECK(Status::Ok == rollback.cancel(0));
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK_FALSE(history.canUndo(0));
	Frame silence{}, output{};
	audio.render(0, {&silence, 1}, {&output, 1});
	CHECK_EQUAL(1, output.l);
}
TEST(LoopHistory, pressureEvictsOldestAndAnIncompleteLatestSessionFencesUndo) {
	CHECK(Status::Ok == history.initialize(std::span(arena).first(kPageBytes)));
	phrase(0, 513);
	dub(0, 1);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	dub(0, 513);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK_FALSE(history.canUndo(0));
	CHECK(history.retainedPages() <= 1);
}
TEST(LoopHistory, pressureFencesOnlyTheSlotWithTheIncompleteStep) {
	CHECK(Status::Ok == history.initialize(std::span(arena).first(kPageBytes)));
	phrase(0);
	phrase(1);
	dub(0);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	dub(1);
	CHECK(Status::Ok == audio.setCaptureGate(1, false));
	// Pressure evicts slot 0's oldest boundary (its initial), leaving the newer
	// overdub navigable; only the slot with the incomplete newest step is fenced.
	CHECK(history.canUndo(0));
	CHECK_FALSE(history.canUndo(1));
}
TEST(LoopHistory, pagePressureRetiresOldBoundaryAndLaterEditsCanCaptureAgain) {
	CHECK(Status::Ok == history.initialize(std::span(arena).first(kPageBytes)));
	auto handle = phrase(0, 513);
	dub(0, 1);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	dub(0, 513); // Exhaust the one before-image page and fence this step.
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK_FALSE(history.canUndo(0));
	for (unsigned i = 0; i < 8; ++i)
		serviceRetirement(16);
	CHECK_EQUAL(0, history.retainedPages());
	dub(0, 1); // A later edit can acquire history after bounded reclaim.
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	CHECK_EQUAL(513, pool.length(handle));
}
TEST(LoopHistory, canceledGestureAndReusedPhraseCannotAdvertiseOldUndo) {
	CHECK(Status::Ok == history.initialize(arena));
	auto handle = phrase();
	CHECK(Status::Ok == rollback.begin(0, 0, 0, 3));
	dub();
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	CHECK(Status::Ok == rollback.cancel(0));
	CHECK_FALSE(history.canUndo(0));
	rollback.service(100);
	audio.stop(0);
	CHECK(Status::Ok == audio.clear(0));
	for (unsigned i = 0; i < 64 && history.retainedSessions(); ++i)
		serviceRetirement();
	CHECK_EQUAL(0, history.retainedSessions());
	phrase();
	// The reused slot legitimately starts a fresh initial boundary; the old
	// overdub history is gone rather than advertised.
	CHECK(history.canUndo(0));
	CHECK_FALSE(history.canRedo(0));
	CHECK_EQUAL(1, history.retainedSessions());
	history.beginSession(kInvalidSlot, handle.generation);
	history.beforeWrite(kInvalidSlot, 0, {});
	history.endSession(kInvalidSlot);
	CHECK_FALSE(history.canUndo(kInvalidSlot));
}
TEST(LoopHistory, emptyBoundariesCoalesceWithoutLosingCheckpointAndStopEndsSession) {
	CHECK(Status::Ok == history.initialize(arena));
	auto handle = phrase();
	dub();
	audio.stop(0);
	CHECK(history.canUndo(0));
	for (unsigned i = 0; i < 100000; ++i) {
		history.beginSession(0, handle.generation);
		history.endSession(0);
		serviceRetirement();
	}
	CHECK(history.canUndo(0));
	CHECK_EQUAL(2, history.retainedSessions()); // Initial boundary plus one overdub.
	CHECK_EQUAL(1, history.retainedPages());
}
TEST(LoopHistory, hundredThousandChangedBoundariesKeepFixedDescriptorsAndPages) {
	CHECK(Status::Ok == history.initialize(arena));
	auto handle = phrase();
	for (unsigned i = 0; i < 100000; ++i) {
		history.beginSession(0, handle.generation);
		history.beforeWrite(0, 0, *pool.frame(handle, 0));
		history.endSession(0);
		serviceRetirement();
	}
	CHECK(history.retainedSessions() <= History::kMaxSessions);
	CHECK(history.retainedPages() <= 4);
}
TEST(LoopHistory, allSlotsKeepOverdubbingUnderHistoryPressure) {
	CHECK(Status::Ok == history.initialize(arena));
	PhraseHandle handles[kTrackCount];
	for (SlotId s = 0; s < kTrackCount; ++s)
		handles[s] = phrase(s);
	for (unsigned round = 0; round < 32; ++round)
		for (SlotId s = 0; s < kTrackCount; ++s)
			dub(s);
	for (SlotId s = 0; s < kTrackCount; ++s) {
		CHECK_EQUAL(97, pool.frame(handles[s], 0)->l);
		audio.stop(s);
	}
	CHECK(history.retainedSessions() <= History::kMaxSessions);
	CHECK(history.retainedPages() <= 4);
}
TEST(LoopHistory, descriptorPressureReusesFixedSlotsWithoutLosingLatestStep) {
	std::vector<std::byte> larger((History::kMaxSessions + 1) * kPageBytes);
	CHECK(Status::Ok == history.initialize(larger));
	auto handle = phrase();
	for (unsigned i = 0; i < 3 * History::kMaxSessions; ++i) {
		history.beginSession(0, handle.generation);
		history.beforeWrite(0, 0, *pool.frame(handle, 0));
		history.endSession(0);
		serviceRetirement();
	}
	CHECK(history.retainedSessions() <= History::kMaxSessions);
	CHECK(history.retainedPages() <= History::kMaxSessions);
	CHECK(history.canUndo(0));
}
TEST(LoopHistory, fullDescriptorsOnOtherTracksRetireOldestReachableAndRecoverCapacity) {
	std::vector<std::byte> larger((History::kMaxSessions + 1) * kPageBytes);
	CHECK(Status::Ok == history.initialize(larger));
	PhraseHandle handles[kTrackCount]{};
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		handles[slot] = phrase(slot);
	// Fill all descriptors on tracks 1–7; track 0 has no history to reset.
	for (SlotId slot = 1; slot < kTrackCount; ++slot) {
		const unsigned sessions = slot == 1 ? 10 : 9;
		for (unsigned i = 0; i < sessions; ++i) {
			dub(slot, 1);
			CHECK(Status::Ok == audio.setCaptureGate(slot, false));
		}
	}
	CHECK_EQUAL(History::kMaxSessions, history.retainedSessions());
	CHECK_FALSE(history.canUndo(0));

	// A new untracked edit on track 0 must retire an oldest live history head
	// elsewhere so bounded foreground work can recover a descriptor.
	history.beginSession(0, handles[0].generation);
	CHECK(history.retirementPending());
	for (unsigned i = 0; i < 10000 && history.retainedSessions() == History::kMaxSessions; ++i)
		serviceRetirement(16);
	CHECK(history.retainedSessions() < History::kMaxSessions);

	// A later edit can now claim a descriptor and retain a fresh undo boundary.
	dub(0, 1);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
}
TEST(LoopHistory, restartAndAttachCloseTheCurrentCaptureSession) {
	CHECK(Status::Ok == history.initialize(arena));
	auto handle = phrase();
	dub();
	CHECK(Status::Ok == audio.start(0, false));
	CHECK(history.canUndo(0));
	dub();
	CHECK(Status::Ok == audio.attach(0, handle));
	CHECK(history.canUndo(0));
}
TEST(LoopHistory, initializeRejectsMisalignmentAndReinitializationButAcceptsSubpageCapacity) {
	CHECK(Status::Invalid == history.initialize(std::span(arena).subspan(1)));
	CHECK(Status::Ok == history.initialize(std::span(arena).first(kPageBytes - 1)));
	CHECK(Status::Busy == history.initialize(arena));
	phrase();
	dub();
	audio.stop(0);
	CHECK_FALSE(history.canUndo(0));
}
TEST(LoopHistory, activeSessionReportsChangedAndCompleteForUndoEligibility) {
	CHECK(Status::Ok == history.initialize(arena));
	phrase(0, 3);
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	CHECK_FALSE(history.activeSessionChanged(0)); // No contributions yet.
	CHECK(history.activeSessionComplete(0));
	const Frame addition{1, -1};
	Frame output{};
	audio.render(0, {&addition, 1}, {&output, 1});
	CHECK(history.activeSessionChanged(0));
	CHECK(history.activeSessionComplete(0));
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK_FALSE(history.activeSessionChanged(0)); // No session is active.
	CHECK_FALSE(history.activeSessionComplete(0));
}
TEST(LoopHistory, activeSessionReportsIncompleteWhenBeforeImagesAreLost) {
	CHECK(Status::Ok == history.initialize({})); // Ready but zero before-image capacity.
	phrase(0, 3);
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	const Frame addition{1, -1};
	Frame output{};
	audio.render(0, {&addition, 1}, {&output, 1});
	CHECK(history.activeSessionChanged(0));
	CHECK_FALSE(history.activeSessionComplete(0)); // The step is fenced rather than skippable.
}
TEST(LoopHistory, InitialUndoToEmptyAndRedoRestoresExactStereoFrames) {
	CHECK(Status::Ok == history.initialize(arena));
	auto base = phrase(0, 5); // Five-frame stereo base, registered as the initial boundary.
	CHECK_EQUAL(1, history.retainedSessions());
	CHECK(history.canUndo(0));
	const auto freeWhileLive = pool.freePages();
	// Initial Undo detaches the recording and leaves the track truly empty,
	// while the retained handle stays readable for Redo.
	CHECK_FALSE(restoreStep(0, false).valid());
	CHECK_FALSE(pool.sealedHandle(0).valid());
	CHECK_EQUAL(5, pool.length(base));
	CHECK_EQUAL(1, pool.frame(base, 0)->l);
	CHECK_EQUAL(-5, pool.frame(base, 4)->r);
	CHECK_FALSE(audio.playing(0));
	CHECK_FALSE(history.canUndo(0));
	CHECK(history.canRedo(0));
	CHECK_EQUAL(freeWhileLive, pool.freePages()); // Retained for Redo, never reclaimed.
	// Redo republishes the exact same stereo frames and stays stopped.
	CHECK(restoreStep(0, true).valid());
	expectFrames(0, {1, 2, 3, 4, 5});
	CHECK_EQUAL(5, pool.length(pool.sealedHandle(0)));
	CHECK_FALSE(audio.playing(0));
	CHECK(history.canUndo(0));
	CHECK_FALSE(history.canRedo(0));
	CHECK_EQUAL(freeWhileLive, pool.freePages());
}
TEST(LoopHistory, OneMultiwrapGateIsOneLayerOverMultipleWraps) {
	CHECK(Status::Ok == history.initialize(arena));
	phrase(0, 5);
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	std::vector<Frame> input(12);
	for (unsigned i = 0; i < input.size(); ++i)
		input[i] = {int32_t(i + 1), -int32_t(i + 1)};
	std::vector<Frame> output(input.size());
	audio.render(0, input, output);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	// One open gate over several wraps is a single layer and one before-image page.
	CHECK_EQUAL(2, history.retainedSessions());
	CHECK_EQUAL(1, history.retainedPages());
	CHECK(history.canUndo(0));
	restoreStep(0, false);
	expectFrames(0, {1, 2, 3, 4, 5});
	CHECK(history.canUndo(0)); // The initial boundary remains the next layer.
	CHECK(history.canRedo(0));
	CHECK_FALSE(restoreStep(0, false).valid());
	CHECK_FALSE(history.canUndo(0));
	CHECK(history.canRedo(0));
}
TEST(LoopHistory, UnchangedSessionCannotUndoPreviousLayer) {
	CHECK(Status::Ok == history.initialize(arena));
	phrase(0, 5);
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	const Frame addition{10, -10};
	Frame output{};
	audio.render(0, {&addition, 1}, {&output, 1});
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK(history.canUndo(0));
	// An empty gate contributes no layer and must not shadow the overdub beneath.
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	serviceRetirement();
	CHECK_EQUAL(2, history.retainedSessions());
	CHECK(history.canUndo(0));
	restoreStep(0, false);
	expectFrames(0, {1, 2, 3, 4, 5});
	CHECK(history.canUndo(0)); // Still the initial boundary, not the unchanged session.
	CHECK(history.canRedo(0));
}
TEST(LoopHistory, NewRecordingAfterEmptyUndoInvalidatesOldForwardBranch) {
	CHECK(Status::Ok == history.initialize(arena));
	phrase(0, 5);
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	const Frame addition{10, -10};
	Frame output{};
	audio.render(0, {&addition, 1}, {&output, 1});
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	restoreStep(0, false); // Undo the overdub.
	restoreStep(0, false); // Undo the initial recording to empty.
	CHECK_FALSE(history.canUndo(0));
	CHECK(history.canRedo(0));
	// A fresh recording starts a new chain and discards the old Redo branch.
	const Frame replacement[] = {{7, -7}, {8, -8}, {9, -9}};
	Frame captured[3];
	CHECK(Status::Ok == audio.beginInitial(0, 3));
	audio.render(0, replacement, captured);
	PhraseHandle fresh{};
	CHECK(Status::Ok == audio.finishInitial(0, fresh));
	CHECK(history.canUndo(0));
	CHECK_FALSE(history.canRedo(0));
	RestoreId stale{};
	CHECK(Status::SourceUnavailable == history.requestRestore(0, true, stale));
	expectFrames(0, {7, 8, 9});
}
TEST(LoopHistory, EvictedInitialBoundaryCannotReappearAfterDescriptorReuse) {
	std::vector<std::byte> larger((History::kMaxSessions + 1) * kPageBytes);
	CHECK(Status::Ok == history.initialize(larger));
	phrase(0, 1);
	const auto freeWithBaseOnly = pool.freePages();
	// Create a retained Redo branch on the overdub, then Undo it so the initial
	// boundary is the cursor while the overdub version is retained.
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	const Frame addition{5, -5};
	Frame output{};
	audio.render(0, {&addition, 1}, {&output, 1});
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	restoreStep(0, false);
	CHECK(history.canUndo(0));
	CHECK(history.canRedo(0));
	// Churn descriptors until the oldest boundary is evicted and its descriptor
	// recycled by later edits.
	const auto generation = pool.sealedHandle(0).generation;
	for (unsigned i = 0; i < 3 * History::kMaxSessions; ++i) {
		history.beginSession(0, generation);
		history.beforeWrite(0, 0, *pool.frame(pool.sealedHandle(0), 0));
		history.endSession(0);
		serviceRetirement();
	}
	CHECK(history.retainedSessions() <= History::kMaxSessions);
	// The evicted initial boundary cannot reappear: no Redo branch survives and
	// its retained version's pages are reclaimed, not leaked.
	CHECK_FALSE(history.canRedo(0));
	RestoreId stale{};
	CHECK(Status::SourceUnavailable == history.requestRestore(0, true, stale));
	for (unsigned i = 0; i < 100 && pool.freePages() != freeWithBaseOnly; ++i)
		serviceRetirement(16);
	CHECK_EQUAL(freeWithBaseOnly, pool.freePages());
}
// Losing a noninitial forward boundary must fence its dependent range: once the
// initial boundary has been evicted under pressure (current audio nonempty) and
// the cursor has been undone back to the start, evicting the next boundary must
// not let a single Redo publish the cumulative later version.
TEST(LoopHistory, NonemptyCurrentRedoCannotSkipAnEvictedInteriorBoundary) {
	CHECK(Status::Ok == history.initialize(std::span(arena).first(2 * kPageBytes)));
	phrase(0, 3); // Initial boundary I.
	dub(0, 1);    // Boundary A captures one before-image page.
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	dub(0, 1); // Boundary B fills the two-page arena.
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	CHECK_EQUAL(2, history.retainedPages()); // The arena is now full.
	CHECK_EQUAL(3, history.retainedSessions());
	// Other-slot pressure during beforeWrite evicts the oldest boundary (the
	// initial I) while the cursor stays on B and the current audio is nonempty.
	phrase(1, 3);
	dub(1, 1);
	CHECK(Status::Ok == audio.setCaptureGate(1, false));
	for (unsigned i = 0; i < 100 && history.retirementPending(); ++i)
		serviceRetirement(16);
	CHECK_EQUAL(3, history.retainedSessions()); // A and B remain; I is gone.
	// Undo B then A: audio is I, cursor none, forward boundary A (single step).
	restoreStep(0, false);
	restoreStep(0, false);
	CHECK(history.canRedo(0)); // Redo A is still exactly one step from I.
	// Descriptor/page pressure on track 1 now evicts the remaining head A.
	CHECK(Status::Ok == audio.setCaptureGate(1, true));
	const Frame addition{7, -7};
	Frame output{};
	audio.render(1, {&addition, 1}, {&output, 1});
	CHECK(Status::Ok == audio.setCaptureGate(1, false));
	// B depends on the removed A: no Redo may publish the cumulative version.
	CHECK_FALSE(history.canRedo(0));
	CHECK_FALSE(history.canUndo(0)); // Cursor is at the start; nothing earlier remains.
	RestoreId redo{};
	CHECK(Status::SourceUnavailable == history.requestRestore(0, true, redo));
	// The retained live audio is untouched and its dependent version is reclaimed.
	CHECK_EQUAL(3, pool.length(pool.sealedHandle(0)));
	CHECK_EQUAL(1, pool.frame(pool.sealedHandle(0), 0)->l);
	for (unsigned i = 0; i < 200 && (history.retirementPending() || pool.retirementPending()); ++i)
		serviceRetirement(16);
	CHECK_FALSE(history.retirementPending());
}
// I4: optional retained history must not starve a new capture. The same slot's
// retained initial is evictable, and a manual take gets a real page grant rather
// than opening a zero-page recording that capacity-completes immediately.
TEST(LoopHistory, captureAdmissionReclaimsSameSlotRetainedInitial) {
	constexpr FrameIndex perPage = kPageBytes / sizeof(Frame);
	CHECK(Status::Ok == history.initialize(arena));
	const auto retained = phrase(0, 16 * perPage);
	CHECK_EQUAL(0, pool.freePages());
	restoreStep(0, false); // Undo the only initial boundary, retaining it for Redo.
	CHECK_EQUAL(0, pool.freePages());
	CHECK_EQUAL(1, history.retainedSessions());
	Status admitted = Status::Busy;
	for (unsigned i = 0; i < 8 && admitted == Status::Busy; ++i)
		admitted = history.reclaimForCapture(0, 0);
	CHECK_EQUAL(int(Status::Ok), int(admitted));
	CHECK(pool.freePages() >= 1);
	CHECK(Status::Ok == audio.beginInitial(0, 0));
	CHECK(Status::Ok == pool.beginRenderRound(audio.manualParticipants()));
	const auto capacity = audio.initialCapacity(0);
	CHECK(capacity.has_value());
	CHECK_EQUAL(int(perPage), int(*capacity)); // A real page was admitted.
	const Frame input{9, -9};
	Frame output{};
	audio.render(0, {&input, 1}, {&output, 1});
	CHECK_EQUAL(1, audio.recordedInitialFrames(0));
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	(void)retained;
}
// I4: cross-slot retained history is reclaimed, while another slot's live phrase
// (never part of a retained optional version) is untouched.
TEST(LoopHistory, captureAdmissionReclaimsCrossSlotInitialWithoutTouchingLivePhrase) {
	constexpr FrameIndex perPage = kPageBytes / sizeof(Frame);
	CHECK(Status::Ok == history.initialize(arena));
	phrase(0, 8 * perPage);
	restoreStep(0, false); // Retain slot 0's initial for Redo.
	const auto live = phrase(1, 8 * perPage);
	CHECK_EQUAL(0, pool.freePages());
	Status admitted = Status::Busy;
	for (unsigned i = 0; i < 8 && admitted == Status::Busy; ++i)
		admitted = history.reclaimForCapture(2, 0);
	CHECK_EQUAL(int(Status::Ok), int(admitted));
	CHECK(pool.freePages() >= 1);
	CHECK_EQUAL(8 * perPage, pool.length(live));
	CHECK_EQUAL(1, pool.frame(live, 0)->l);
	CHECK_EQUAL(-1, pool.frame(live, 0)->r);
	CHECK(audio.playing(1));
	CHECK(Status::Ok == audio.beginInitial(2, 0));
}
// I4: a retained overdub after-version is evictable for another slot's capture,
// but the currently published phrase remains exact and playing.
TEST(LoopHistory, captureAdmissionReclaimsRetainedOverdubAndProtectsCurrentAudio) {
	constexpr FrameIndex perPage = kPageBytes / sizeof(Frame);
	CHECK(Status::Ok == history.initialize(arena));
	phrase(0, 6 * perPage);
	dub(0, 1);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	restoreStep(0, false); // Undo the overdub: previous version is retained, current restored.
	const auto live = pool.sealedHandle(0);
	CHECK(live.valid());
	CHECK_EQUAL(6 * perPage, pool.length(live));
	CHECK_EQUAL(1, pool.frame(live, 0)->l);
	CHECK(audio.playing(0));
	phrase(1, 4 * perPage); // Fill the remaining live pages with a protected live phrase.
	CHECK_EQUAL(0, pool.freePages());
	Status admitted = Status::Busy;
	unsigned attempts = 0;
	while (admitted == Status::Busy && attempts < 16) {
		admitted = history.reclaimForCapture(2, 5 * perPage);
		++attempts;
	}
	CHECK_EQUAL(int(Status::Ok), int(admitted));
	CHECK(attempts >= 2); // More than one bounded foreground quantum.
	CHECK(pool.freePages() >= 5);
	// Current audio (and the other slot's live phrase) stayed owned and exact.
	CHECK_EQUAL(6 * perPage, pool.length(live));
	CHECK_EQUAL(1, pool.frame(live, 0)->l);
	CHECK_EQUAL(-1, pool.frame(live, 0)->r);
	CHECK(audio.playing(0));
	CHECK_EQUAL(4 * perPage, pool.length(pool.sealedHandle(1)));
	CHECK(audio.playing(1));
	CHECK(Status::Ok == audio.beginInitial(2, 5 * perPage));
}
// I4: optional retained descriptors are reclaimed before a new initial take, so
// admission is not refused merely because reachable history owns the table.
TEST(LoopHistory, captureAdmissionReclaimsRetainedDescriptor) {
	constexpr FrameIndex perPage = kPageBytes / sizeof(Frame);
	std::vector<std::byte> larger(64 * kPageBytes);
	CHECK(Status::Ok == history.initialize(larger));
	phrase(0, perPage);
	for (unsigned i = 0; i < 63; ++i) {
		CHECK(Status::Ok == audio.setCaptureGate(0, true));
		const Frame input{3, -3};
		Frame output{};
		audio.render(0, {&input, 1}, {&output, 1});
		CHECK(Status::Ok == audio.setCaptureGate(0, false));
	}
	CHECK_EQUAL(64, history.retainedSessions());
	CHECK_FALSE(history.canAdmitInitialBoundary());
	Status admitted = Status::Busy;
	for (unsigned i = 0; i < 8 && admitted == Status::Busy; ++i)
		admitted = history.reclaimForCapture(1, 0);
	CHECK_EQUAL(int(Status::Ok), int(admitted));
	CHECK(history.canAdmitInitialBoundary());
	CHECK_EQUAL(63, history.retainedSessions());
	CHECK(Status::Ok == audio.beginInitial(1, 0));
}
// I4: a pinned snapshot keeps its version readable; capture admission reports
// protected exhaustion instead of reclaiming it, then succeeds after unpin.
TEST(LoopHistory, captureAdmissionPreservesPinnedSnapshotUntilReleased) {
	constexpr FrameIndex perPage = kPageBytes / sizeof(Frame);
	CHECK(Status::Ok == history.initialize(arena));
	std::vector<std::byte> snapshotBytes(4 * kPageBytes);
	CHECK(Status::Ok == snapshots.initialize(snapshotBytes));
	const auto handle = phrase(0, 16 * perPage);
	SnapshotId id;
	CHECK(Status::Ok == snapshots.request(0, handle.generation, id));
	CHECK(Status::Ok == snapshots.preserve(0));
	restoreStep(0, false); // Retain the pinned initial version for Redo.
	CHECK_EQUAL(0, pool.freePages());
	Status admitted = Status::Busy;
	for (unsigned i = 0; i < 8 && admitted == Status::Busy; ++i)
		admitted = history.reclaimForCapture(0, 0);
	CHECK_EQUAL(int(Status::NoMemory), int(admitted)); // Pinned pages are protected.
	CHECK_EQUAL(16 * perPage, pool.length(handle));
	std::array<Frame, 4> staging{};
	CHECK(Status::Ok == snapshots.read(id, 0, staging));
	CHECK_EQUAL(1, staging[0].l);
	CHECK_EQUAL(-1, staging[0].r);
	CHECK_EQUAL(-4, staging[3].r);
	snapshots.release(id);
	for (unsigned i = 0; i < 8 && admitted != Status::Ok; ++i)
		admitted = history.reclaimForCapture(0, 0);
	CHECK_EQUAL(int(Status::Ok), int(admitted));
	CHECK(pool.freePages() >= 1);
	CHECK(Status::Ok == audio.beginInitial(0, 0));
}
