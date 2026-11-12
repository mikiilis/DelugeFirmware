#include "probe_support.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int failures = 0;

void check(bool condition, char const* expression, int line) {
	if (!condition) {
		std::fprintf(stderr, "line %d: %s\n", line, expression);
		++failures;
	}
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void fillCanaries(Canvas& canvas, uint8_t value = 0xa5) {
	canvas.leadingCanary.fill(value);
	canvas.trailingCanary.fill(value);
}

void checkCanaries(const Canvas& canvas, uint8_t value = 0xa5) {
	for (auto byte : canvas.leadingCanary)
		CHECK(byte == value);
	for (auto byte : canvas.trailingCanary)
		CHECK(byte == value);
}

uint64_t imageHash(const Canvas& canvas) {
	uint64_t hash = 1469598103934665603ull;
	for (const auto& row : canvas.image_) {
		for (uint8_t byte : row) {
			hash ^= byte;
			hash *= 1099511628211ull;
		}
	}
	return hash;
}

void testInBoundsRenderingMatchesGoldenImages() {
	Canvas text;
	text.clear();
	text.drawString("Loop Station", 0, 6, 9, 10);
	CHECK(imageHash(text) == 0xaf7bb17108502fffull);

	constexpr uint8_t bitmap[] = {0x81, 0x42, 0x24, 0x18, 0xff, 0x00};
	Canvas regular;
	regular.clear();
	regular.drawGraphicMultiLine(bitmap, 11, 7, 3, 16, 2);
	CHECK(imageHash(regular) == 0xecd04eeacc73bb8bull);

	Canvas clippedMirror;
	clippedMirror.clear();
	clippedMirror.drawGraphicMultiLine(bitmap, 127, 7, 3, 16, 2, true);
	CHECK(imageHash(clippedMirror) == 0x9c0adfee7de3ddcaull);
}

void testOffscreenUndoLabelDoesNotWrite() {
	Canvas canvas;
	canvas.clear();
	fillCanaries(canvas, 0);
	canvas.drawString("Undo unavailable", 0, 48, 16, 12);
	checkCanaries(canvas, 0);
	for (const auto& row : canvas.image_)
		for (auto byte : row)
			CHECK(byte == 0);
}

void testBitmapOutsideOriginsAreNoOps() {
	constexpr std::array<uint8_t, 2> bitmap{0x01, 0x02};
	constexpr std::array<std::array<int32_t, 2>, 5> origins{{{0, 48}, {0, 64}, {128, 0}, {-1, 0}, {0, -1}}};
	for (auto [x, y] : origins) {
		for (bool reversed : {false, true}) {
			Canvas canvas;
			canvas.clear();
			fillCanaries(canvas);
			canvas.drawGraphicMultiLine(bitmap.data(), x, y, 2, 8, 1, reversed);
			checkCanaries(canvas);
			for (const auto& row : canvas.image_)
				for (auto byte : row)
					CHECK(byte == 0);
		}
	}
}

void testEmptyBitmapIsNoOp() {
	constexpr uint8_t bitmap[] = {0xff};
	struct Case {
		int32_t width, height, stride;
		uint8_t const* source;
	};
	const std::array cases{Case{0, 8, 1, bitmap}, Case{-1, 8, 1, bitmap}, Case{1, 0, 1, bitmap}, Case{1, -1, 1, bitmap},
	                       Case{1, 8, 0, bitmap}, Case{1, 8, -1, bitmap}, Case{1, 8, 1, nullptr}};
	for (const auto& test : cases) {
		for (bool reversed : {false, true}) {
			Canvas canvas;
			canvas.clear();
			fillCanaries(canvas);
			canvas.drawGraphicMultiLine(test.source, 0, 0, test.width, test.height, test.stride, reversed);
			checkCanaries(canvas);
			for (const auto& row : canvas.image_)
				for (auto byte : row)
					CHECK(byte == 0);
		}
	}
}

void testBottomRightClipPreservesMemory() {
	constexpr uint8_t bitmap[] = {0xff, 0xff};
	Canvas canvas;
	canvas.clear();
	fillCanaries(canvas);
	canvas.drawGraphicMultiLine(bitmap, 127, 47, 2, 8, 1);
	for (int y = 0; y < 6; ++y)
		for (int x = 0; x < 128; ++x)
			CHECK(canvas.image_[y][x] == ((y == 5 && x == 127) ? 0x80 : 0));
	checkCanaries(canvas);
}

void testMirroredBitmapRetainsOutput() {
	constexpr uint8_t bitmap[] = {0x01, 0x02};
	Canvas canvas;
	canvas.clear();
	fillCanaries(canvas);
	canvas.drawGraphicMultiLine(bitmap, 0, 0, 2, 8, 1, true);
	CHECK(canvas.image_[0][0] == 0x02);
	CHECK(canvas.image_[0][1] == 0x01);
	checkCanaries(canvas);
}
} // namespace

int main() {
	testOffscreenUndoLabelDoesNotWrite();
	if (failures) {
		std::fprintf(stderr, "%d OLED canvas checks failed\n", failures);
		return EXIT_FAILURE;
	}
	testInBoundsRenderingMatchesGoldenImages();
	testBitmapOutsideOriginsAreNoOps();
	testEmptyBitmapIsNoOp();
	testBottomRightClipPreservesMemory();
	testMirroredBitmapRetainsOutput();
	if (failures) {
		std::fprintf(stderr, "%d OLED canvas checks failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("OledCanvasTests passed");
	return EXIT_SUCCESS;
}
