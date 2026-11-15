#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/length_display.h"

#include <cstring>

using namespace loop_station;

namespace {
void expect(FrameIndex frames, uint32_t tempoQ16, const char* expected) {
	char out[24];
	formatPhraseLength(frames, tempoQ16, out, sizeof(out));
	STRCMP_EQUAL(expected, out);
	CHECK(std::strlen(out) <= 8);
}
} // namespace

TEST_GROUP(LoopLengthDisplay){};

TEST(LoopLengthDisplay, recognizesMusicalLengthsWithoutSnappingFreeTakes) {
	const struct {
		FrameIndex frames;
		const char* expected;
	} cases[] = {
	    {0, "0.00 s"},      {1, "<0.01 s"},       {220, "<0.01 s"},   {221, "0.01 s"},
	    {1838, "1/48 BAR"}, {2757, "1/32 BAR"},   {3675, "1/24 BAR"}, {5513, "1/16 BAR"},
	    {7350, "1/12 BAR"}, {11025, "1/8 BAR"},   {14700, "1/6 BAR"}, {16538, "3/16 BAR"},
	    {22050, "1/4 BAR"}, {29400, "1/3 BAR"},   {44100, "1/2 BAR"}, {66150, "3/4 BAR"},
	    {88200, "1 BAR"},   {132300, "3/2 BARS"}, {352800, "4 BARS"}, {16317, "0.37 s"},
	    {5511, "0.12 s"},   {5512, "1/16 BAR"},   {5514, "1/16 BAR"}, {5515, "0.13 s"},
	    {330750, "7.50 s"}, // 15/4 BARS exceeds the eight-character field.
	};
	for (const auto& c : cases)
		expect(c.frames, 120U * 65536U, c.expected);
}

TEST(LoopLengthDisplay, fractionalTempoAndTempoChangesUseRawQ16) {
	expect(5880, 112U * 65536U + 32768U, "1/16 BAR");
	expect(5513, 121U * 65536U, "0.13 s");
	expect(16317, 60U * 65536U, "0.37 s");
	expect(16317, 120U * 65536U, "0.37 s");
}

TEST(LoopLengthDisplay, secondsCarryAndLargeInputsAreBounded) {
	expect(439, 0, "0.01 s");
	expect(44099, 0, "1.00 s");
	expect(44099779, 0, "999.99 s");
	expect(44099780, 0, "1000 s");
	expect(UINT32_MAX, 0, "97392 s");
	expect(UINT32_MAX, 1000U * 65536U, "97392 s");
	expect(16317, 1001U * 65536U, "0.37 s");
	expect(16317, 1U, "0.37 s");
}

TEST(LoopLengthDisplay, truncatedDestinationPreservesCanariesAndTerminator) {
	char guarded[] = {'A', 'x', 'x', 'x', 'x', 'Z'};
	formatPhraseLength(5513, 120U * 65536U, guarded + 1, 4);
	STRCMP_EQUAL("1/1", guarded + 1);
	CHECK_EQUAL('A', guarded[0]);
	CHECK_EQUAL('Z', guarded[5]);

	char oneByte[] = {'A', 'x', 'Z'};
	formatPhraseLength(5513, 120U * 65536U, oneByte + 1, 1);
	CHECK_EQUAL('\0', oneByte[1]);
	CHECK_EQUAL('A', oneByte[0]);
	CHECK_EQUAL('Z', oneByte[2]);
}

TEST(LoopLengthDisplay, zeroSizeDoesNotAccessDestination) {
	formatPhraseLength(1, 0, nullptr, 0);
	char untouched = 'x';
	formatPhraseLength(1, 0, &untouched, 0);
	CHECK_EQUAL('x', untouched);
}
