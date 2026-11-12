#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/gesture_rollback.h"
#include "deluge/loop_station/resident_audio.h"

#include <array>
#include <limits>
#include <vector>

using namespace loop_station;

TEST_GROUP(LoopGestureRollback) {
	std::vector<std::byte> backing;
	std::vector<std::byte> scratch;
	PagePool pool;
	GestureRollback rollback{pool};
	ResidentAudio audio{pool, rollback};
	void setup() {
		backing.resize(64 * kPageBytes);
		scratch.resize(GestureRollback::requiredBytes());
		CHECK(Status::Ok == pool.initialize(backing));
		CHECK(Status::Ok == rollback.initialize(scratch));
	}
	PhraseHandle phrase(FrameIndex length = 1, SlotId slot = 0, Frame value = {10, -20}) {
		std::vector<Frame> frames(length, value);
		CHECK(Status::Ok == pool.beginPhrase(slot, length));
		FrameIndex accepted = 0;
		CHECK(Status::Ok == pool.append(slot, frames, accepted));
		CHECK_EQUAL(length, accepted);
		PhraseHandle handle;
		CHECK(Status::Ok == pool.seal(slot, handle));
		CHECK(Status::Ok == audio.attach(slot, handle));
		CHECK(Status::Ok == audio.start(slot, false));
		return handle;
	}
	void begin(SlotId slot = 0, SampleClock now = 0, FrameIndex length = 1) {
		CHECK(Status::Ok == rollback.begin(slot, now, audio.position(slot), length));
		CHECK(Status::Ok == audio.setCaptureGate(slot, true));
	}
	void samples(unsigned count, SlotId slot = 0, Frame value = {1, -1}) {
		std::array<Frame, 137> input;
		input.fill(value);
		std::array<Frame, 137> output;
		while (count) {
			const auto n = std::min<unsigned>(count, input.size());
			audio.render(slot, std::span(input).first(n), std::span(output).first(n));
			count -= n;
		}
	}
	Frame audible(SlotId slot = 0) {
		CHECK(Status::Ok == audio.setCaptureGate(slot, false));
		const Frame input[1]{};
		Frame output[1];
		audio.render(slot, input, output);
		return output[0];
	}
};

// Missing first-write protection loses the original after thousands of saturated wraps.
TEST(LoopGestureRollback, saturatedSingleFrameCancelIsImmediatelyAudibleAndRestoresExactlyOnce) {
	const auto handle = phrase();
	begin();
	samples(12000, 0, {INT32_MAX, INT32_MIN});
	CHECK_EQUAL(INT32_MAX, pool.frame(handle, 0)->l);
	CHECK_EQUAL(1, rollback.work().capturedFrames);
	CHECK(Status::Ok == rollback.cancel(0));
	CHECK_FALSE(rollback.provisional(0));
	CHECK_EQUAL(10, audible().l);
	CHECK_EQUAL(-20, audible().r);
	CHECK_EQUAL(INT32_MAX, pool.frame(handle, 0)->l);
	rollback.service(1);
	CHECK_EQUAL(10, pool.frame(handle, 0)->l);
	CHECK_EQUAL(-20, pool.frame(handle, 0)->r);
	CHECK_EQUAL(1, rollback.work().restoredFrames);
	CHECK(Status::Ok == rollback.cancel(0));
	rollback.service(100);
	CHECK_EQUAL(1, rollback.work().restoredFrames);
}

TEST(LoopGestureRollback, confirmationKeepsSaturatedAdditionsAndSecondCancelIsHarmless) {
	const auto handle = phrase();
	begin();
	samples(10000, 0, {INT32_MAX, INT32_MIN});
	CHECK(Status::Ok == rollback.confirm(0));
	CHECK(Status::Ok == rollback.cancel(0));
	rollback.service(15435);
	CHECK_EQUAL(INT32_MAX, audible().l);
	CHECK_EQUAL(INT32_MIN, pool.frame(handle, 0)->r);
	CHECK_EQUAL(0, rollback.work().restoredFrames);
}

TEST(LoopGestureRollback, expiresAtSample15435IncludingInsideLargeRenderWindow) {
	phrase();
	begin(0, 700);
	samples(15434);
	CHECK(rollback.provisional(0));
	samples(1);
	CHECK_FALSE(rollback.provisional(0));
	CHECK(Status::Ok == rollback.cancel(0));
	CHECK_EQUAL(15445, audible().l);
}

TEST(LoopGestureRollback, authoritativeClockExpiresStoppedAndClosedCaptureAtExactBoundary) {
	phrase();
	begin(0, 900);
	samples(3);
	audio.stop(0);
	rollback.advance(16334);
	CHECK(rollback.provisional(0));
	rollback.advance(16335);
	CHECK_FALSE(rollback.provisional(0));
	rollback.advance(0); // An old observation cannot reopen or extend a gesture.
	CHECK(Status::Ok == rollback.cancel(0));
}

TEST(LoopGestureRollback, allEightSlotsProtectIndependentlyWithoutHistory) {
	PhraseHandle handles[kTrackCount];
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		handles[slot] = phrase(1, slot, {100 + slot, -100 - slot});
		begin(slot);
		samples(1000 + slot, slot);
	}
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		CHECK(Status::Ok == rollback.cancel(slot));
		CHECK_EQUAL(100 + slot, audible(slot).l);
	}
	rollback.service(7);
	CHECK_EQUAL(7, rollback.work().restoredFrames);
	rollback.service(1);
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		CHECK_EQUAL(-100 - slot, pool.frame(handles[slot], 0)->r);
}

TEST(LoopGestureRollback, newCaptureDuringRestorationPreservesOriginalAndRetainsConfirmedNewInput) {
	const auto handle = phrase(1000);
	begin(0, 0, 1000);
	samples(900);
	CHECK(Status::Ok == rollback.cancel(0));
	rollback.service(2);
	begin(0, 900, 1000);
	samples(700, 0, {3, -4});
	CHECK(Status::Ok == rollback.confirm(0));
	rollback.service(15435);
	for (unsigned i = 0; i < 1000; ++i) {
		const bool changed = i < 600 || i >= 900;
		CHECK_EQUAL(changed ? 13 : 10, pool.frame(handle, i)->l);
		CHECK_EQUAL(changed ? -24 : -20, pool.frame(handle, i)->r);
	}
}

TEST(LoopGestureRollback, repeatedCancelStartCannotExhaustOrReuseReferencedScratch) {
	const auto handle = phrase(20000);
	begin(0, 0, 20000);
	samples(15434);
	CHECK(Status::Ok == rollback.cancel(0));
	for (unsigned n = 0; n < 20000; ++n) {
		begin(0, 15434 + n, 20000);
		samples(1, 0, {INT32_MAX, INT32_MIN});
		CHECK(Status::Ok == rollback.cancel(0));
	}
	rollback.service(15435);
	for (unsigned i = 0; i < 20000; ++i) {
		CHECK_EQUAL(10, pool.frame(handle, i)->l);
		CHECK_EQUAL(-20, pool.frame(handle, i)->r);
	}
}

TEST(LoopGestureRollback, initialCaptureBindsProtectionBeforeFirstPlaybackGesture) {
	CHECK(Status::Ok == audio.beginInitial(0, 3));
	const Frame input[] = {{10, -20}, {30, -40}, {50, -60}};
	Frame output[3];
	audio.render(0, input, output);
	PhraseHandle handle;
	CHECK(Status::Ok == audio.finishInitial(0, handle));
	CHECK(Status::Ok == audio.start(0, false));
	begin(0, 0, 3);
	samples(100);
	CHECK(Status::Ok == rollback.cancel(0));
	rollback.service(3);
	CHECK_EQUAL(10, pool.frame(handle, 0)->l);
	CHECK_EQUAL(-60, pool.frame(handle, 2)->r);
}

TEST(LoopGestureRollback, pendingOldGenerationCannotBeReboundOrReplacedUntilRetired) {
	const auto handle = phrase();
	begin();
	samples(1);
	CHECK(Status::Ok == rollback.cancel(0));
	CHECK(Status::Ok == pool.release(handle));
	CHECK(Status::Busy == audio.beginInitial(0, 1));
	rollback.service(1);
	CHECK(Status::Ok == audio.beginInitial(0, 1));
	samples(1, 0, {70, -80});
	PhraseHandle replacement;
	CHECK(Status::Ok == audio.finishInitial(0, replacement));
	CHECK_EQUAL(70, pool.frame(replacement, 0)->l);
}

TEST(LoopGestureRollback, oversizedSingleRenderExpiresBeforeTheNextSampleIsCaptured) {
	const auto handle = phrase(16000);
	begin(0, 0, 16000);
	std::vector<Frame> input(16000, {2, -3});
	std::vector<Frame> output(16000);
	audio.render(0, input, output);
	CHECK_FALSE(rollback.provisional(0));
	CHECK_EQUAL(15435, rollback.work().capturedFrames);
	CHECK_EQUAL(12, pool.frame(handle, 15999)->l);
	CHECK(Status::Ok == rollback.cancel(0));
	rollback.service(15435);
	CHECK_EQUAL(12, pool.frame(handle, 15434)->l);
	CHECK_EQUAL(12, pool.frame(handle, 15435)->l);
}

TEST(LoopGestureRollback, partialRestoreAndSecondCancelKeepBothDisjointOriginalMappings) {
	const auto handle = phrase(1000);
	begin(0, 0, 1000);
	samples(900);
	CHECK(Status::Ok == rollback.cancel(0));
	begin(0, 900, 1000);
	samples(2, 0, {5, -8});
	CHECK(Status::Ok == rollback.cancel(0));
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	const std::array<Frame, 1000> input{};
	std::array<Frame, 1000> output;
	audio.render(0, input, output);
	for (const auto frame : output) {
		CHECK_EQUAL(10, frame.l);
		CHECK_EQUAL(-20, frame.r);
	}
	rollback.service(2000);
	CHECK_EQUAL(10, pool.frame(handle, 900)->l);
}

TEST(LoopGestureRollback, invalidStorageSlotsPhasesLengthsAndClockCannotDamageLiveMapping) {
	GestureRollback other(pool);
	CHECK(Status::NoMemory == other.initialize(std::span(scratch).first(1)));
	std::vector<std::byte> unaligned(GestureRollback::requiredBytes() + 1);
	CHECK(Status::Invalid == other.initialize(std::span(unaligned).subspan(1)));
	CHECK(Status::Busy == rollback.initialize(scratch));
	CHECK(Status::Invalid == rollback.begin(kInvalidSlot, 0, 0, 1));
	CHECK(Status::Stale == rollback.begin(0, 0, 0, 1));
	phrase(3);
	CHECK(Status::Invalid == rollback.begin(0, 0, 3, 3));
	CHECK(Status::Invalid == rollback.begin(0, 0, 0, 2));
	begin(0, 100, 3);
	CHECK(Status::Busy == rollback.begin(0, 100, 0, 3));
	samples(1);
	CHECK(Status::Ok == rollback.cancel(0));
	CHECK(Status::Invalid == rollback.begin(0, 99, 0, 3));
	CHECK(Status::Invalid == rollback.cancel(kInvalidSlot));
	CHECK(Status::Invalid == rollback.confirm(kInvalidSlot));
	CHECK_FALSE(rollback.provisional(kInvalidSlot));
	rollback.beforeWrite(kInvalidSlot, UINT32_MAX, {});
	CHECK_EQUAL(77, rollback.read(kInvalidSlot, 0, {77, 88}).l);
	CHECK_EQUAL(10, audible().l);
}

TEST(LoopGestureRollback, randomizedOverlapsAndRetirementMatchIndependentAudioModel) {
	constexpr unsigned length = 17011;
	const auto handle = phrase(length);
	std::vector<Frame> model(length, {10, -20});
	std::vector<Frame> original;
	SampleClock now = 0;
	unsigned position = 0;
	uint32_t random = 0x9d143c71;
	auto next = [&]() {
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		return random;
	};
	for (unsigned gesture = 0; gesture < 400; ++gesture) {
		original = model;
		begin(0, now, length);
		const unsigned count = 1 + next() % 330;
		for (unsigned i = 0; i < count; ++i) {
			const Frame input[1] = {
			    {static_cast<int32_t>(next() % 2001) - 1000, static_cast<int32_t>(next() % 2001) - 1000}};
			Frame output[1];
			const auto before = rollback.work();
			audio.render(0, input, output);
			CHECK_EQUAL(model[position].l, output[0].l);
			CHECK_EQUAL(model[position].r, output[0].r);
			CHECK(rollback.work().treeSteps - before.treeSteps <= 1024);
			CHECK(rollback.work().retiredFrames - before.retiredFrames <= 2);
			CHECK(rollback.work().capturedFrames - before.capturedFrames <= 1);
			model[position].l += input[0].l;
			model[position].r += input[0].r;
			position = (position + 1) % length;
			++now;
		}
		const auto before = rollback.work();
		if (next() & 1) {
			CHECK(Status::Ok == rollback.cancel(0));
			model = original;
		}
		else
			CHECK(Status::Ok == rollback.confirm(0));
		CHECK_EQUAL(before.treeSteps, rollback.work().treeSteps);
		const unsigned credits = next() % 11;
		rollback.service(credits);
		CHECK(rollback.work().retiredFrames - before.retiredFrames <= credits);
		CHECK(rollback.work().treeSteps - before.treeSteps <= 256 * credits);
	}
	rollback.service(15435);
	for (unsigned i = 0; i < length; ++i) {
		CHECK_EQUAL(model[i].l, pool.frame(handle, i)->l);
		CHECK_EQUAL(model[i].r, pool.frame(handle, i)->r);
	}
}

TEST(LoopGestureRollback, fullArenaReuseAndServiceHaveConstantPerCallWorkAtPageAndPhraseBoundaries) {
	const auto handle = phrase(20000);
	begin(0, 0, 20000);
	const Frame input[1] = {{1, -1}};
	Frame output[1];
	for (unsigned i = 0; i < 15435; ++i) {
		const auto before = rollback.work();
		audio.render(0, input, output);
		CHECK(rollback.work().treeSteps - before.treeSteps <= 1024);
	}
	CHECK_FALSE(rollback.provisional(0));
	CHECK_EQUAL(15435, rollback.work().capturedFrames);
	begin(0, 15435, 20000);
	for (unsigned i = 0; i < 15434; ++i) {
		const auto before = rollback.work();
		audio.render(0, input, output);
		CHECK(rollback.work().treeSteps - before.treeSteps <= 1024);
	}
	CHECK(Status::Ok == rollback.cancel(0));
	for (unsigned i = 0; i < 15435; ++i) {
		const auto before = rollback.work();
		rollback.service(1);
		CHECK_EQUAL(1, rollback.work().retiredFrames - before.retiredFrames);
		CHECK(rollback.work().treeSteps - before.treeSteps <= 256);
	}
	CHECK_FALSE(rollback.pending(0));
	CHECK_EQUAL(11, pool.frame(handle, 0)->l);
	CHECK_EQUAL(11, pool.frame(handle, 15434)->l);
	CHECK_EQUAL(10, pool.frame(handle, 15435)->l);
}

TEST(LoopGestureRollback, uninitializedProtectionRejectsInitialAdmissionWithoutConsumingBacking) {
	GestureRollback uninitialized(pool);
	ResidentAudio unprotectedAudio(pool, uninitialized);
	const auto freePages = pool.freePages();
	CHECK(Status::Invalid == unprotectedAudio.beginInitial(0, 1));
	CHECK_EQUAL(freePages, pool.freePages());
	const Frame input[] = {{100, -200}};
	Frame output[1] = {{99, 99}};
	unprotectedAudio.render(0, input, output);
	CHECK_EQUAL(0, output[0].l);
	CHECK(Status::Invalid == unprotectedAudio.start(0, false));
	CHECK(Status::Invalid == unprotectedAudio.setCaptureGate(0, true));
	const auto handle = phrase();
	CHECK(Status::Invalid == unprotectedAudio.attach(0, handle));
	CHECK(Status::Invalid == unprotectedAudio.start(0, false));
	CHECK(Status::Invalid == unprotectedAudio.setCaptureGate(0, true));
	unprotectedAudio.render(0, input, output);
	CHECK_EQUAL(10, pool.frame(handle, 0)->l);
}

TEST(LoopGestureRollback, failedInitialBindingCannotAdmitPlaybackOrCapture) {
	// An independently initialized pool makes sealing succeed while the protection
	// binding fails. The completion error must remain an admission failure.
	alignas(Frame) std::byte otherStorage[kPageBytes]{};
	PagePool otherPool;
	CHECK(Status::Ok == otherPool.initialize(otherStorage));
	GestureRollback otherRollback(otherPool);
	std::vector<std::byte> otherScratch(GestureRollback::requiredBytes());
	CHECK(Status::Ok == otherRollback.initialize(otherScratch));
	ResidentAudio failingAudio(pool, otherRollback);
	CHECK(Status::Ok == failingAudio.beginInitial(0, 1));
	const Frame input[] = {{100, -200}};
	Frame output[1];
	failingAudio.render(0, input, output);
	CHECK(Status::Stale == failingAudio.takeCompletion(0).status);
	CHECK(Status::Stale == failingAudio.start(0, false));
	CHECK(Status::Invalid == failingAudio.setCaptureGate(0, true));
	failingAudio.render(0, input, output);
	CHECK_EQUAL(0, output[0].l);
	CHECK_EQUAL(0, output[0].r);
}
