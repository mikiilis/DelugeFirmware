#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/transpose.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace loop_station;

TEST_GROUP(LoopTranspose){};

TEST(LoopTranspose, unityIsBitExactBypass) {
	Transpose transpose;
	CHECK(Status::Ok == transpose.prepare(0));
	transpose.setCents(0, 0);
	std::array<Frame, 32> frames{};
	for (size_t i = 0; i < frames.size(); ++i)
		frames[i] = {int32_t(i * 0x10203u), -int32_t(i * 0x20101u)};
	const auto original = frames;
	transpose.process(0, frames);
	MEMCMP_EQUAL(original.data(), frames.data(), sizeof(frames));
}

TEST(LoopTranspose, extremeCentRatiosUseRoundedQ24UnityConvention) {
	Transpose transpose;
	CHECK(Status::Ok == transpose.prepare(0));
	transpose.setCents(0, 2450);
	const auto high = uint32_t(std::floor(std::pow(2.0, 2450.0 / 1200.0) * (1u << 24) + 0.5));
	CHECK_EQUAL(high, transpose.ratioQ24(0));
	transpose.setCents(0, -2450);
	const auto low = uint32_t(std::floor(std::pow(2.0, -2450.0 / 1200.0) * (1u << 24) + 0.5));
	CHECK_EQUAL(low, transpose.ratioQ24(0));
}

TEST(LoopTranspose, centsOutsideRangeClampAtBothEnds) {
	Transpose transpose;
	CHECK(Status::Ok == transpose.prepare(0));
	transpose.setCents(0, 2451);
	CHECK_EQUAL(2450, transpose.cents(0));
	transpose.setCents(0, -4000);
	CHECK_EQUAL(-2450, transpose.cents(0));
}

TEST(LoopTranspose, releaseMakesSlotUnavailableAndPrepareRestoresIt) {
	Transpose transpose;
	CHECK(Status::Ok == transpose.prepare(0));
	transpose.setCents(0, 1200);
	transpose.release(0);
	std::array<Frame, 8> frames{};
	CHECK_FALSE(transpose.process(0, frames));
	CHECK(Status::Ok == transpose.prepare(0));
	CHECK_EQUAL(0, transpose.cents(0));
	CHECK_EQUAL(1u << 24, transpose.ratioQ24(0));
}

TEST(LoopTranspose, octaveUpMovesCapturedSineNearDoubleFrequency) {
	constexpr uint32_t sampleRate = 48000;
	constexpr double sourceHz = 440.0;
	constexpr size_t totalFrames = sampleRate;
	constexpr size_t blockFrames = 128;
	Transpose transpose;
	CHECK(Status::Ok == transpose.prepare(0));
	transpose.setCents(0, 1200);
	std::vector<Frame> rendered(totalFrames);
	for (size_t offset = 0; offset < rendered.size(); offset += blockFrames) {
		const auto count = std::min(blockFrames, rendered.size() - offset);
		for (size_t i = 0; i < count; ++i) {
			const auto t = double(offset + i) / sampleRate;
			const auto sample = int32_t(std::sin(2.0 * M_PI * sourceHz * t) * 1000000000.0);
			rendered[offset + i] = {sample, sample};
		}
		CHECK(transpose.process(0, std::span(rendered).subspan(offset, count)));
	}
	const size_t measureFrom = 12000;
	const size_t measureTo = rendered.size() - 1;
	unsigned crossings = 0;
	for (size_t i = measureFrom + 1; i <= measureTo; ++i)
		if (rendered[i - 1].l <= 0 && rendered[i].l > 0)
			++crossings;
	const auto estimatedHz = double(crossings) * sampleRate / double(measureTo - measureFrom);
	CHECK(estimatedHz > 800.0);
	CHECK(estimatedHz < 960.0);
}

TEST(LoopTranspose, ratioEditKeepsTheAlreadyBufferedGrainStream) {
	Transpose transpose;
	CHECK(Status::Ok == transpose.prepare(0));
	transpose.setCents(0, 1200);
	std::array<Frame, 128> frames{};
	uint32_t inputPosition = 0;
	for (unsigned block = 0; block < 22; ++block) {
		for (auto& frame : frames) {
			const auto sample = int32_t(inputPosition++ * 10000u);
			frame = {sample, sample};
		}
		CHECK(transpose.process(0, frames));
	}
	transpose.setCents(0, -1200); // Land during the first overlapping-grain handoff.
	for (auto& frame : frames) {
		const auto sample = int32_t(inputPosition++ * 10000u);
		frame = {sample, sample};
	}
	const auto firstInput = frames.front().l;
	CHECK(transpose.process(0, frames));
	CHECK(firstInput != frames.front().l);
	for (size_t i = 1; i < frames.size(); ++i) {
		const int64_t delta = int64_t(frames[i].l) - frames[i - 1].l;
		CHECK(std::abs(delta) < 1000000000);
	}
}

TEST(LoopTranspose, unityBypassIsExactAndResetsBufferedHistory) {
	Transpose transpose;
	CHECK(Status::Ok == transpose.prepare(0));
	transpose.setCents(0, 1200);
	std::array<Frame, 128> frames{};
	for (unsigned block = 0; block < 40; ++block) {
		frames.fill({100000000, -100000000});
		CHECK(transpose.process(0, frames));
	}
	transpose.setCents(0, 0);
	frames.fill({123456789, -987654321});
	const auto original = frames;
	CHECK(transpose.process(0, frames));
	MEMCMP_EQUAL(original.data(), frames.data(), sizeof(frames));
	transpose.setCents(0, 1200);
	frames.fill({0, 0});
	CHECK(transpose.process(0, frames));
	for (const auto frame : frames) {
		CHECK_EQUAL(0, frame.l);
		CHECK_EQUAL(0, frame.r);
	}
}

TEST(LoopTranspose, discontinuityDropsThePreviousStreamHistory) {
	Transpose transpose;
	CHECK(Status::Ok == transpose.prepare(0));
	transpose.setCents(0, 1200);
	std::array<Frame, 128> frames{};
	for (unsigned block = 0; block < 40; ++block) {
		frames.fill({100000000, -100000000});
		CHECK(transpose.process(0, frames));
	}
	frames.fill({0, 0});
	CHECK(transpose.process(0, frames, true));
	for (const auto frame : frames) {
		CHECK_EQUAL(0, frame.l);
		CHECK_EQUAL(0, frame.r);
	}
}
