#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/gesture_rollback.h"
#include "deluge/loop_station/resident_audio.h"

#include <array>
#include <bit>
#include <limits>
#include <vector>

using namespace loop_station;

TEST_GROUP(LoopAudio) {
	alignas(Frame) std::byte storage[4 * kPageBytes]{};
	PagePool pool;
	GestureRollback rollback{pool};
	ResidentAudio audio{pool, rollback};
	std::vector<std::byte> scratch;
	void setup() {
		CHECK(Status::Ok == pool.initialize(storage));
		scratch.resize(GestureRollback::requiredBytes());
		CHECK(Status::Ok == rollback.initialize(scratch));
	}
	PhraseHandle phrase(std::span<const Frame> frames, SlotId slot = 0) {
		CHECK(Status::Ok == pool.beginPhrase(slot, frames.size()));
		FrameIndex accepted = 0;
		CHECK(Status::Ok == pool.append(slot, frames, accepted));
		CHECK_EQUAL(frames.size(), accepted);
		PhraseHandle handle;
		CHECK(Status::Ok == pool.seal(slot, handle));
		CHECK(Status::Ok == audio.attach(slot, handle));
		return handle;
	}
};

TEST(LoopAudio, continuousDubReturnsBackingBeforeAdditionAndSilencePreservesIt) {
	const Frame backing[] = {{10, -10}, {20, -20}, {30, -30}};
	const auto handle = phrase(backing);
	CHECK(Status::Ok == audio.start(0, false));
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	const Frame input[] = {{1, -1}, {2, -2}, {3, -3}, {4, -4}, {5, -5}, {6, -6}, {7, -7}};
	Frame output[7]{};
	audio.render(0, input, output);
	const int expected[] = {10, 20, 30, 11, 22, 33, 15};
	for (unsigned i = 0; i < 7; ++i) {
		CHECK_EQUAL(expected[i], output[i].l);
		CHECK_EQUAL(-expected[i], output[i].r);
	}
	const int sum[] = {22, 27, 39};
	const Frame silence[7]{};
	audio.render(0, silence, output);
	for (unsigned i = 0; i < 3; ++i) {
		CHECK_EQUAL(sum[i], pool.frame(handle, i)->l);
		CHECK_EQUAL(-sum[i], pool.frame(handle, i)->r);
	}
}

TEST(LoopAudio, contentRevisionAdvancesForEveryOverdubWriteAndClear) {
	const Frame backing[] = {{10, 20}, {30, 40}};
	phrase(backing);
	CHECK_EQUAL(0, audio.contentRevision(0));
	CHECK(Status::Ok == audio.start(0, false));
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	const Frame input[] = {{1, 2}, {3, 4}, {5, 6}};
	Frame output[3]{};
	audio.render(0, input, output);
	CHECK_EQUAL(3, audio.contentRevision(0));
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	audio.stop(0);
	CHECK(Status::Ok == audio.clear(0));
	CHECK_EQUAL(4, audio.contentRevision(0));
}

TEST(LoopAudio, gateCloseExcludesNextInputAndStopIsImmediate) {
	const Frame backing[] = {{10, 20}, {30, 40}, {50, 60}};
	const auto handle = phrase(backing);
	CHECK(Status::Ok == audio.start(0, false));
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	const Frame input[] = {{1, 2}};
	Frame output[1]{};
	audio.render(0, input, output);
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	audio.render(0, input, output);
	CHECK_EQUAL(30, output[0].l);
	CHECK_EQUAL(30, pool.frame(handle, 1)->l);
	audio.stop(0);
	audio.render(0, input, output);
	CHECK_EQUAL(0, output[0].l);
	CHECK_EQUAL(2, audio.position(0));
	CHECK_EQUAL(50, pool.frame(handle, 2)->l);
}

TEST(LoopAudio, saturatesBothChannelsWithoutOverflow) {
	constexpr auto hi = std::numeric_limits<int32_t>::max();
	constexpr auto lo = std::numeric_limits<int32_t>::min();
	const Frame backing[] = {{hi - 1, lo + 1}, {lo, hi}};
	const auto handle = phrase(backing);
	CHECK(Status::Ok == audio.start(0, false));
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	const Frame input[] = {{10, -10}, {lo, hi}};
	Frame output[2]{};
	audio.render(0, input, output);
	CHECK_EQUAL(hi, pool.frame(handle, 0)->l);
	CHECK_EQUAL(lo, pool.frame(handle, 0)->r);
	CHECK_EQUAL(lo, pool.frame(handle, 1)->l);
	CHECK_EQUAL(hi, pool.frame(handle, 1)->r);
}

TEST(LoopAudio, one_shot_dub_wraps_until_gate_closes) {
	const Frame backing[] = {{10, -10}, {20, -20}, {30, -30}};
	const auto handle = phrase(backing);
	for (bool oneShot : {true, false}) {
		CHECK(Status::Ok == audio.start(0, oneShot));
		CHECK(Status::Ok == audio.setCaptureGate(0, true));
		const Frame input[8] = {{1, -1}, {1, -1}, {1, -1}, {1, -1}, {1, -1}, {1, -1}, {1, -1}, {1, -1}};
		Frame output[8]{};
		audio.render(0, input, output);
		CHECK_EQUAL(2, audio.position(0));
		CHECK(output[7].l != 0);
		const Frame before = *pool.frame(handle, 2);
		CHECK(Status::Ok == audio.setCaptureGate(0, false));
		audio.render(0, input, std::span(output).first(2));
		CHECK_EQUAL(before.l, output[0].l);
		CHECK_EQUAL(before.r, output[0].r);
		CHECK_EQUAL(oneShot ? 0 : pool.frame(handle, 0)->l, output[1].l);
		CHECK_EQUAL(before.l, pool.frame(handle, 2)->l);
		CHECK_EQUAL(oneShot ? 0 : 1, audio.position(0));
		auto completion = audio.takeCompletion(0);
		CHECK(completion.reason == (oneShot ? CompletionReason::OneShot : CompletionReason::None));
		CHECK(audio.takeCompletion(0).reason == CompletionReason::None);
		CHECK(Status::Ok == audio.start(0, oneShot));
		CHECK(Status::Ok == audio.setCaptureGate(0, true));
		audio.render(0, std::span(input).first(3), std::span(output).first(3));
		CHECK_EQUAL(0, audio.position(0));
		CHECK(Status::Ok == audio.setCaptureGate(0, false));
		audio.render(0, input, std::span(output).first(4));
		CHECK(output[2].l != 0);
		CHECK_EQUAL(oneShot ? 0 : pool.frame(handle, 0)->l, output[3].l);
	}
}

TEST(LoopAudio, initialStartsAtZeroAndClosesBeforeNextSample) {
	CHECK(Status::Ok == pool.beginRenderRound(1));
	CHECK(Status::Ok == audio.beginInitial(0, 0));
	const Frame input[] = {{11, 12}, {21, 22}, {31, 32}};
	Frame output[3];
	audio.render(0, input, output);
	for (const auto frame : output) {
		CHECK_EQUAL(0, frame.l);
		CHECK_EQUAL(0, frame.r);
	}
	CHECK(Status::Ok == audio.setCaptureGate(0, false));
	audio.render(0, input, output);
	PhraseHandle handle;
	CHECK(Status::Ok == audio.finishInitial(0, handle));
	CHECK_EQUAL(3, pool.length(handle));
	CHECK(Status::Ok == audio.start(0, false));
	audio.render(0, input, output);
	CHECK_EQUAL(11, output[0].l);
	CHECK_EQUAL(32, output[2].r);
}

TEST(LoopAudio, exactDurationCompletesAndExcludesExtraInput) {
	CHECK(Status::Ok == audio.beginInitial(0, 3));
	const Frame input[] = {{1, -1}, {2, -2}, {3, -3}, {999, -999}};
	Frame output[4];
	audio.render(0, input, output);
	auto event = audio.takeCompletion(0);
	CHECK(event.reason == CompletionReason::Duration);
	CHECK_EQUAL(3, event.frames);
	PhraseHandle handle;
	CHECK(Status::Ok == audio.finishInitial(0, handle));
	CHECK_EQUAL(3, pool.length(handle));
	CHECK_EQUAL(3, pool.frame(handle, 2)->l);
	POINTERS_EQUAL(nullptr, pool.frame(handle, 3));
	CHECK_EQUAL(0, output[3].l);
}

TEST(LoopAudio, emptyAndStalePhrasesNeverStart) {
	CHECK(Status::Invalid == audio.start(0, false));
	CHECK(Status::Invalid == audio.attach(0, {}));
	CHECK(Status::Invalid == audio.beginInitial(kInvalidSlot, 0));
	CHECK(Status::Invalid == audio.setCaptureGate(kInvalidSlot, true));
	CHECK(Status::Ok == audio.beginInitial(0, 1));
	PhraseHandle empty;
	CHECK(Status::Invalid == audio.finishInitial(0, empty));
	CHECK_FALSE(empty.valid());
	CHECK(Status::Invalid == audio.start(0, false));
	const Frame backing[] = {{1, 2}};
	const auto handle = phrase(backing);
	CHECK(Status::Ok == pool.release(handle));
	CHECK_EQUAL(0, pool.length(handle));
	CHECK_EQUAL(0, pool.length({}));
	CHECK(Status::Stale == audio.attach(0, handle));
	CHECK(Status::Stale == audio.start(0, false));
}

TEST(LoopAudio, twoSecondsOfNativePlaybackHasNoPhaseDriftAcrossPages) {
	std::array<Frame, 777> backing;
	for (unsigned i = 0; i < backing.size(); ++i)
		backing[i] = {static_cast<int>(i + 1), -static_cast<int>(i + 1)};
	phrase(backing);
	CHECK(Status::Ok == audio.start(0, false));
	std::array<Frame, 137> silence{};
	std::array<Frame, 137> output;
	unsigned elapsed = 0;
	while (elapsed < 2 * kSampleRate) {
		const unsigned count = std::min<unsigned>(silence.size(), 2 * kSampleRate - elapsed);
		audio.render(0, std::span(silence).first(count), std::span(output).first(count));
		for (unsigned i = 0; i < count; ++i) {
			const int expected = (elapsed + i) % 777 + 1;
			CHECK_EQUAL(expected, output[i].l);
			CHECK_EQUAL(-expected, output[i].r);
		}
		elapsed += count;
	}
	CHECK_EQUAL(399, audio.position(0));
}

TEST(LoopAudio, capacityCompletionPreservesEveryAcceptedFrame) {
	alignas(Frame) std::byte smallStorage[kPageBytes]{};
	PagePool smallPool;
	GestureRollback smallRollback(smallPool);
	std::vector<std::byte> smallScratch(GestureRollback::requiredBytes());
	CHECK(Status::Ok == smallRollback.initialize(smallScratch));
	ResidentAudio smallAudio(smallPool, smallRollback);
	CHECK(Status::Ok == smallPool.initialize(smallStorage));
	CHECK(Status::Ok == smallPool.beginRenderRound(1));
	CHECK(Status::Ok == smallAudio.beginInitial(0, 0));
	std::array<Frame, 513> input;
	for (unsigned i = 0; i < input.size(); ++i)
		input[i] = {static_cast<int>(i + 1), -static_cast<int>(i + 1)};
	std::array<Frame, 513> output;
	smallAudio.render(0, input, output);
	const auto event = smallAudio.takeCompletion(0);
	CHECK(event.reason == CompletionReason::Capacity);
	CHECK_EQUAL(512, event.frames);
	PhraseHandle handle;
	CHECK(Status::Ok == smallAudio.finishInitial(0, handle));
	CHECK_EQUAL(512, smallPool.length(handle));
	for (unsigned i = 0; i < 512; ++i) {
		CHECK_EQUAL(input[i].l, smallPool.frame(handle, i)->l);
		CHECK_EQUAL(input[i].r, smallPool.frame(handle, i)->r);
	}
	CHECK(Status::Ok == smallAudio.start(0, true));
	smallAudio.render(0, input, output);
	CHECK_EQUAL(1, output[0].l);
	CHECK_EQUAL(-512, output[511].r);
	CHECK_EQUAL(0, output[512].l);
}

TEST(LoopAudio, manualRendersNeverRestartSharedRoundOrStealOtherGrants) {
	CHECK(Status::Ok == pool.beginRenderRound(3));
	CHECK(Status::Ok == audio.beginInitial(0, 0));
	CHECK(Status::Ok == audio.beginInitial(1, 0));
	std::array<Frame, 513> input;
	input.fill({7, -9});
	std::array<Frame, 513> output;
	audio.render(0, input, output);
	audio.render(1, input, output);
	CHECK_EQUAL(2, pool.freePages());
	for (SlotId slot = 0; slot < 2; ++slot) {
		const auto completion = audio.takeCompletion(slot);
		CHECK(completion.reason == CompletionReason::Capacity);
		CHECK_EQUAL(512, completion.frames);
		PhraseHandle handle;
		CHECK(Status::Ok == audio.finishInitial(slot, handle));
		CHECK_EQUAL(512, pool.length(handle));
		CHECK_EQUAL(-9, pool.frame(handle, 511)->r);
	}
}

TEST(LoopAudio, zeroAcceptedAdmissionFailureReturnsNoHandle) {
	alignas(Frame) std::byte smallStorage[kPageBytes]{};
	PagePool smallPool;
	GestureRollback smallRollback(smallPool);
	std::vector<std::byte> smallScratch(GestureRollback::requiredBytes());
	CHECK(Status::Ok == smallRollback.initialize(smallScratch));
	ResidentAudio smallAudio(smallPool, smallRollback);
	CHECK(Status::Ok == smallPool.initialize(smallStorage));
	CHECK(Status::Ok == smallPool.beginRenderRound(3));
	CHECK(Status::Ok == smallAudio.beginInitial(1, 0));
	const Frame input[] = {{1, 2}};
	Frame output[1]{};
	smallAudio.render(1, input, output);
	const auto event = smallAudio.takeCompletion(1);
	CHECK(event.reason == CompletionReason::Capacity);
	CHECK(event.status == Status::Busy);
	CHECK_EQUAL(0, event.frames);
	PhraseHandle handle;
	CHECK(Status::Invalid == smallAudio.finishInitial(1, handle));
	CHECK_FALSE(handle.valid());
	CHECK(Status::Invalid == smallAudio.start(1, false));
}

TEST(LoopAudio, exhaustionOnLaterRenderKeepsAllEarlierAcceptedInput) {
	alignas(Frame) std::byte smallStorage[kPageBytes]{};
	PagePool smallPool;
	GestureRollback smallRollback(smallPool);
	std::vector<std::byte> smallScratch(GestureRollback::requiredBytes());
	CHECK(Status::Ok == smallRollback.initialize(smallScratch));
	ResidentAudio smallAudio(smallPool, smallRollback);
	CHECK(Status::Ok == smallPool.initialize(smallStorage));
	CHECK(Status::Ok == smallPool.beginRenderRound(1));
	CHECK(Status::Ok == smallAudio.beginInitial(0, 0));
	std::array<Frame, 512> input;
	input.fill({17, -23});
	std::array<Frame, 512> output;
	smallAudio.render(0, input, output);
	CHECK(smallAudio.takeCompletion(0).reason == CompletionReason::None);
	CHECK(Status::Ok == smallPool.beginRenderRound(1));
	smallAudio.render(0, std::span(input).first(1), std::span(output).first(1));
	const auto event = smallAudio.takeCompletion(0);
	CHECK(event.reason == CompletionReason::Capacity);
	CHECK(event.status == Status::NoMemory);
	CHECK_EQUAL(512, event.frames);
	PhraseHandle handle;
	CHECK(Status::Ok == smallAudio.finishInitial(0, handle));
	CHECK_EQUAL(512, smallPool.length(handle));
	CHECK_EQUAL(17, smallPool.frame(handle, 0)->l);
	CHECK_EQUAL(-23, smallPool.frame(handle, 511)->r);
}

TEST(LoopAudio, fixedDurationStopsOnExactPageAndSpanBoundaries) {
	for (FrameIndex duration : {1u, 511u, 512u, 513u, 1024u, 1537u}) {
		CHECK(Status::Ok == audio.beginInitial(0, duration));
		std::array<Frame, 71> input;
		input.fill({101, -202});
		std::array<Frame, 71> output;
		FrameIndex rendered = 0;
		while (rendered < duration) {
			const auto count = std::min<FrameIndex>(input.size(), duration - rendered);
			audio.render(0, std::span(input).first(count), std::span(output).first(count));
			rendered += count;
		}
		const auto event = audio.takeCompletion(0);
		CHECK(event.reason == CompletionReason::Duration);
		CHECK_EQUAL(duration, event.frames);
		CHECK(event.status == Status::Ok);
		audio.render(0, input, output);
		PhraseHandle handle;
		CHECK(Status::Ok == audio.finishInitial(0, handle));
		CHECK_EQUAL(duration, pool.length(handle));
		CHECK_EQUAL(-202, pool.frame(handle, duration - 1)->r);
		CHECK(Status::Ok == pool.release(handle));
	}
}

TEST(LoopAudio, stopInitialSealsAndReservationFailureLeavesExistingPhraseIntact) {
	CHECK(Status::NoMemory == audio.beginInitial(0, 2049));
	CHECK_EQUAL(4, pool.freePages());
	CHECK(Status::Ok == audio.beginInitial(0, 100));
	CHECK(Status::Busy == audio.start(0, false));
	const Frame input[] = {{4, -5}};
	Frame output[1];
	audio.render(0, input, output);
	audio.stop(0);
	audio.render(0, input, output);
	CHECK_EQUAL(0, output[0].l);
	PhraseHandle handle;
	CHECK(Status::Ok == audio.finishInitial(0, handle));
	CHECK_EQUAL(1, pool.length(handle));
	CHECK(Status::Busy == audio.beginInitial(0, 1));
	CHECK_EQUAL(4, pool.frame(handle, 0)->l);
	CHECK(Status::Ok == audio.start(0, false));
	audio.render(0, input, output);
	CHECK_EQUAL(4, output[0].l);
}

TEST(LoopAudio, invalidSlotsStaleHandlesAndUnequalSpansStayBounded) {
	const Frame backing[] = {{1, 2}, {3, 4}};
	const auto handle = phrase(backing);
	CHECK(Status::Invalid == audio.attach(1, handle));
	CHECK(Status::Invalid == audio.start(kInvalidSlot, false));
	PhraseHandle invalid;
	CHECK(Status::Invalid == audio.finishInitial(kInvalidSlot, invalid));
	CHECK_EQUAL(0, audio.position(kInvalidSlot));
	audio.stop(kInvalidSlot);
	CHECK(audio.takeCompletion(kInvalidSlot).reason == CompletionReason::None);
	CHECK(Status::Ok == audio.start(0, false));
	Frame output[3] = {{99, 99}, {99, 99}, {99, 99}};
	audio.render(0, std::span(backing).first(1), output);
	CHECK_EQUAL(1, output[0].l);
	CHECK_EQUAL(0, output[1].l);
	CHECK_EQUAL(0, output[2].r);
	CHECK_EQUAL(1, audio.position(0));
	CHECK(Status::Ok == pool.release(handle));
	audio.render(0, backing, output);
	CHECK_EQUAL(0, output[0].l);
	CHECK(Status::Invalid == audio.setCaptureGate(0, true));
	audio.render(kInvalidSlot, backing, output);
	CHECK_EQUAL(0, output[1].r);
}

TEST(LoopAudio, inPlaceDryBufferStillCapturesOriginalInput) {
	const Frame backing[] = {{10, 20}, {30, 40}};
	const auto handle = phrase(backing);
	CHECK(Status::Ok == audio.start(0, false));
	CHECK(Status::Ok == audio.setCaptureGate(0, true));
	Frame buffer[] = {{1, 2}, {3, 4}};
	audio.render(0, buffer, buffer);
	CHECK_EQUAL(10, buffer[0].l);
	CHECK_EQUAL(40, buffer[1].r);
	CHECK_EQUAL(11, pool.frame(handle, 0)->l);
	CHECK_EQUAL(44, pool.frame(handle, 1)->r);
}

TEST(LoopAudio, randomizedLongDubsMatchIndependentSampleModel) {
	uint32_t randomState = 0x8c032a17;
	auto next = [&]() {
		randomState ^= randomState << 13;
		randomState ^= randomState >> 17;
		randomState ^= randomState << 5;
		return randomState;
	};
	auto sample = [&]() { return std::bit_cast<int32_t>(next()); };
	auto saturated = [](int32_t a, int32_t b) {
		const int64_t value = int64_t(a) + int64_t(b);
		if (value > INT32_MAX)
			return INT32_MAX;
		if (value < INT32_MIN)
			return INT32_MIN;
		return static_cast<int32_t>(value);
	};
	for (unsigned length : {1u, 3u, 511u, 512u, 513u, 1001u}) {
		std::vector<Frame> model(length);
		for (auto& frame : model)
			frame = {sample(), sample()};
		const auto handle = phrase(model);
		bool oneShot = next() & 1;
		bool playing = true;
		bool capture = true;
		unsigned phase = 0;
		CHECK(Status::Ok == audio.start(0, oneShot));
		CHECK(Status::Ok == audio.setCaptureGate(0, capture));
		std::array<Frame, 193> input;
		std::array<Frame, 193> output;
		for (unsigned block = 0; block < 1200; ++block) {
			if (!playing || next() % 29 == 0) {
				oneShot = next() & 1;
				CHECK(Status::Ok == audio.start(0, oneShot));
				phase = 0;
				playing = true;
				capture = false;
			}
			if (next() % 4 == 0) {
				capture = next() & 1;
				CHECK(Status::Ok == audio.setCaptureGate(0, capture));
			}
			const unsigned count = 1 + next() % input.size();
			for (unsigned i = 0; i < count; ++i)
				input[i] = {sample(), sample()};
			audio.render(0, std::span(input).first(count), std::span(output).first(count));
			for (unsigned i = 0; i < count; ++i) {
				if (!playing) {
					CHECK_EQUAL(0, output[i].l);
					CHECK_EQUAL(0, output[i].r);
					continue;
				}
				CHECK_EQUAL(model[phase].l, output[i].l);
				CHECK_EQUAL(model[phase].r, output[i].r);
				if (capture) {
					model[phase].l = saturated(model[phase].l, input[i].l);
					model[phase].r = saturated(model[phase].r, input[i].r);
				}
				if (++phase == length) {
					phase = 0;
					if (oneShot && !capture)
						playing = false;
				}
			}
			CHECK_EQUAL(phase, audio.position(0));
		}
		for (unsigned i = 0; i < length; ++i) {
			CHECK_EQUAL(model[i].l, pool.frame(handle, i)->l);
			CHECK_EQUAL(model[i].r, pool.frame(handle, i)->r);
		}
		audio.stop(0);
		CHECK(Status::Ok == pool.release(handle));
	}
}

TEST(LoopAudio, lengthRejectsReleasedAndReusedHandleGenerations) {
	const Frame backing[] = {{1, 2}, {3, 4}};
	const auto first = phrase(backing);
	CHECK_EQUAL(2, pool.length(first));
	CHECK(Status::Ok == pool.release(first));
	const auto second = phrase(std::span(backing).first(1));
	CHECK_EQUAL(0, pool.length(first));
	CHECK_EQUAL(1, pool.length(second));
	CHECK_EQUAL(0, pool.length({kInvalidSlot, 1}));
}

TEST(LoopAudio, finishInitialRejectsReleasedResultHandle) {
	CHECK(Status::Ok == audio.beginInitial(0, 1));
	const Frame input[] = {{1, 2}};
	Frame output[1];
	audio.render(0, input, output);
	PhraseHandle handle;
	CHECK(Status::Ok == audio.finishInitial(0, handle));
	CHECK(Status::Ok == pool.release(handle));
	CHECK(Status::Stale == audio.finishInitial(0, handle));
	CHECK_FALSE(handle.valid());
}
