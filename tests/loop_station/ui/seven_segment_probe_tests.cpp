// Exercises the extracted production seven-segment encoder, the real scrolling
// layer progression and the real layer lifecycle. A four-character basic popup
// cannot show the track/action/instruction; this proves the persistent scrolling
// path does.

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace deluge::hid::display;

namespace {
int failures = 0;

void check(bool condition, char const* expression, int line) {
	if (!condition) {
		std::fprintf(stderr, "line %d: %s\n", line, expression);
		++failures;
	}
}

#define CHECK(expression) check((expression), #expression, __LINE__)
#define CHECK_FALSE(expression) check(!(expression), "!(" #expression ")", __LINE__)

constexpr const char* kUndo = "UNDO TR 1 BACK=OK";    // 17 encoded positions
constexpr const char* kRedo = "REDO TR 1 SH+BACK=OK"; // 21 encoded positions

void encode(const char* text, uint8_t* out) {
	SevenSegment seg;
	seg.encodeText(text, out, false, {}, true, 0);
}

void testLongConfirmationEncodesPastDisplayLength() {
	SevenSegment seg;
	uint8_t destination[64]{};
	const int32_t length = seg.encodeText(kUndo, destination, false, {}, false, 0);
	// The full track/action/instruction is encoded, not truncated to four chars.
	CHECK(length == static_cast<int32_t>(std::strlen(kUndo)));
	CHECK(length > kNumericDisplayLength);
}

void testBasicPopupWouldTruncateToDisplayLength() {
	// The previously used basic popup path stops at the display length, which is
	// exactly why the track and instruction never appeared. The scrolling path
	// above encodes the whole string instead.
	SevenSegment seg;
	uint8_t destination[kNumericDisplayLength]{};
	const int32_t length = seg.encodeText(kUndo, destination, false, {}, true, 0);
	CHECK(length == kNumericDisplayLength);
	CHECK(length < static_cast<int32_t>(std::strlen(kUndo)));
}

void testScrollingLayerRevealsTrackAndInstruction() {
	SevenSegment seg;
	NumericLayerScrollingText* layer = seg.setScrollingText(kUndo, 0, 600, -1);
	CHECK(layer != nullptr);
	CHECK(layer == seg.topLayer);
	CHECK(layer->length == static_cast<uint16_t>(std::strlen(kUndo)));
	CHECK(layer->scrollsCount == -1); // persistent, never self-terminating

	uint8_t window[kNumericDisplayLength]{};
	uint8_t expected[kNumericDisplayLength]{};
	layer->render(window);
	encode("UNDO", expected);
	CHECK(std::memcmp(window, expected, kNumericDisplayLength) == 0); // the action

	// Advance the real timer callback until the instruction tail is on screen.
	for (int i = 0; i < 200 && layer->currentPos < 10; ++i)
		CHECK_FALSE(layer->callBack());
	CHECK(layer->currentPos == 10);
	layer->render(window);
	encode("BACK", expected);
	CHECK(std::memcmp(window, expected, kNumericDisplayLength) == 0); // the instruction

	// A persistent layer never reports that it finished.
	for (int i = 0; i < 100; ++i)
		CHECK_FALSE(layer->callBack());
}

void testRedoInstructionAlsoScrolls() {
	SevenSegment seg;
	NumericLayerScrollingText* layer = seg.setScrollingText(kRedo, 0, 600, -1);
	CHECK(layer->length == static_cast<uint16_t>(std::strlen(kRedo)));
	CHECK(layer->length > kNumericDisplayLength);
	layer->currentPos = 10;
	uint8_t window[kNumericDisplayLength]{};
	uint8_t expected[kNumericDisplayLength]{};
	layer->render(window);
	encode("SH+B", expected);
	CHECK(std::memcmp(window, expected, kNumericDisplayLength) == 0);
}

void testOwningLayerIsReplacedNotAccumulated() {
	SevenSegment seg;
	NumericLayerScrollingText* first = seg.setScrollingText(kUndo, 0, 600, -1);
	CHECK(seg.topLayer == first);
	NumericLayerScrollingText* second = seg.setScrollingText(kRedo, 0, 600, -1);
	CHECK(second != first);
	CHECK(seg.topLayer == second);
	CHECK(second->next == nullptr); // the previous owned layer was removed
}

void testSetTextRemovesOwnedScrollingLayer() {
	SevenSegment seg;
	NumericLayerScrollingText* layer = seg.setScrollingText(kUndo, 0, 600, -1);
	CHECK(seg.topLayer == layer);
	seg.setText("LOOP"); // the resting call the view makes when it cancels
	CHECK(seg.topLayer != layer);
	CHECK(seg.topLayer->next == nullptr);
	CHECK(dynamic_cast<NumericLayerBasicText*>(seg.topLayer) != nullptr);

	uint8_t frame[kNumericDisplayLength]{};
	uint8_t expected[kNumericDisplayLength]{};
	seg.topLayer->render(frame);
	encode("LOOP", expected);
	CHECK(std::memcmp(frame, expected, kNumericDisplayLength) == 0);
}

void testOverlayInstallFailureAndCleanupSurviveAllocatorFailure() {
	SevenSegment seg;
	CHECK(seg.installScrollingOverlay(kUndo) == nullptr); // no resting layer to overlay
	seg.setText("LOOP");
	NumericLayer* base = seg.topLayer;
	GeneralMemoryAllocator::get().failNextAllocation = true;
	CHECK(seg.installScrollingOverlay(kUndo) == nullptr);
	CHECK(seg.topLayer == base);
	// Install failure is retryable once the allocator recovers.
	NumericLayerScrollingText* layer = seg.installScrollingOverlay(kUndo);
	CHECK(layer != nullptr);
	CHECK(seg.topLayer == layer);
	CHECK(layer->next == base);
	// Cleanup is allocation-free and keeps the resting layer and popup owner.
	seg.popupActive = true;
	GeneralMemoryAllocator::get().failAllocations = true;
	CHECK(seg.removeLayer(layer));
	CHECK(seg.popupActive);
	CHECK(seg.topLayer == base);
	CHECK(dynamic_cast<NumericLayerBasicText*>(seg.topLayer) != nullptr);
	CHECK_FALSE(seg.removeLayer(base)); // never remove the resting layer
	GeneralMemoryAllocator::get().failAllocations = false;
	seg.popupActive = false;
}

void testOverlayCleanupRemovesOnlyTheExactOwnedLayer() {
	SevenSegment seg;
	seg.setText("BASE");
	NumericLayerBasicText* base = dynamic_cast<NumericLayerBasicText*>(seg.topLayer);
	NumericLayerScrollingText* owned = seg.installScrollingOverlay(kUndo);
	CHECK(owned != nullptr);
	NumericLayerScrollingText* newer = seg.installScrollingOverlay(kRedo);
	CHECK(newer != nullptr);
	CHECK(seg.topLayer == newer);
	// The view's exact older overlay is removed without disturbing the newer
	// unrelated layer or the base beneath it.
	CHECK(seg.removeLayer(owned));
	CHECK(seg.topLayer == newer);
	CHECK(newer->next == base);
	CHECK_FALSE(seg.removeLayer(owned));
	CHECK(seg.removeLayer(newer));
	CHECK(seg.topLayer == base);
	CHECK_FALSE(seg.removeLayer(nullptr));
}
} // namespace

int main() {
	testLongConfirmationEncodesPastDisplayLength();
	testBasicPopupWouldTruncateToDisplayLength();
	testScrollingLayerRevealsTrackAndInstruction();
	testRedoInstructionAlsoScrolls();
	testOwningLayerIsReplacedNotAccumulated();
	testSetTextRemovesOwnedScrollingLayer();
	testOverlayInstallFailureAndCleanupSurviveAllocatorFailure();
	testOverlayCleanupRemovesOnlyTheExactOwnedLayer();
	if (failures) {
		std::fprintf(stderr, "%d seven-segment checks failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("LoopStationSevenSegmentProbe passed");
	return EXIT_SUCCESS;
}
