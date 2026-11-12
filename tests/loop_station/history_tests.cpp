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
	CHECK_EQUAL(1, history.retainedSessions());
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
	CHECK_FALSE(history.canUndo(0));
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
	serviceRetirement();
	phrase();
	CHECK_FALSE(history.canUndo(0));
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
	CHECK_EQUAL(1, history.retainedSessions());
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
