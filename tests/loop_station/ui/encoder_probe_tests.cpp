#include <cstdio>
#include <cstdlib>

UI* probeCurrentUI = nullptr;

namespace {
int failures = 0;

void check(bool condition, char const* expression, int line) {
	if (!condition) {
		std::fprintf(stderr, "line %d: %s\n", line, expression);
		++failures;
	}
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void testUnrelatedNotificationReachesCurrentUiAndNativeTempoPath() {
	UI ordinary;
	probeCurrentUI = &ordinary;
	instrumentClipView.tempoCalls = 0;
	playbackHandler.tempoCalls = 0;
	runtimeFeatureSettings.quantize = RuntimeFeatureStateToggle::Off;
	Buttons::tempoEncDown = true;
	Buttons::shiftDown = true;
	tempoEncoderDispatch(-3);
	CHECK(ordinary.notifications == 1);
	CHECK(playbackHandler.tempoCalls == 1);
	CHECK(playbackHandler.lastDelta == -3);
	CHECK(playbackHandler.lastButton == true);
	CHECK(playbackHandler.lastShift == true);
	CHECK(instrumentClipView.tempoCalls == 0);
}

void testInstrumentClipQuantizeKeepsClipHandlerAndNotifies() {
	probeCurrentUI = &instrumentClipView;
	instrumentClipView.notifications = 0;
	instrumentClipView.tempoCalls = 0;
	playbackHandler.tempoCalls = 0;
	runtimeFeatureSettings.quantize = RuntimeFeatureStateToggle::On;
	Buttons::tempoEncDown = false;
	Buttons::shiftDown = false;
	tempoEncoderDispatch(2);
	CHECK(instrumentClipView.notifications == 1);
	CHECK(instrumentClipView.tempoCalls == 1);
	CHECK(instrumentClipView.lastDelta == 2);
	CHECK(playbackHandler.tempoCalls == 0); // native clip routing preserved
}

void testAutomationNoteEditorQuantizeUsesClipHandler() {
	probeCurrentUI = &automationView;
	automationView.noteEditor = true;
	automationView.notifications = 0;
	instrumentClipView.tempoCalls = 0;
	playbackHandler.tempoCalls = 0;
	runtimeFeatureSettings.quantize = RuntimeFeatureStateToggle::On;
	tempoEncoderDispatch(1);
	CHECK(automationView.notifications == 1);
	CHECK(instrumentClipView.tempoCalls == 1);
	CHECK(playbackHandler.tempoCalls == 0);
}

void testInstrumentClipWithoutQuantizeFallsBackToTempo() {
	probeCurrentUI = &instrumentClipView;
	instrumentClipView.notifications = 0;
	instrumentClipView.tempoCalls = 0;
	playbackHandler.tempoCalls = 0;
	runtimeFeatureSettings.quantize = RuntimeFeatureStateToggle::Off;
	tempoEncoderDispatch(4);
	CHECK(instrumentClipView.notifications == 1);
	CHECK(instrumentClipView.tempoCalls == 0);
	CHECK(playbackHandler.tempoCalls == 1);
	CHECK(playbackHandler.lastDelta == 4);
}
} // namespace

int main() {
	testUnrelatedNotificationReachesCurrentUiAndNativeTempoPath();
	testInstrumentClipQuantizeKeepsClipHandlerAndNotifies();
	testAutomationNoteEditorQuantizeUsesClipHandler();
	testInstrumentClipWithoutQuantizeFallsBackToTempo();
	if (failures) {
		std::fprintf(stderr, "%d tempo encoder checks failed\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("LoopStationEncoderProbe passed");
	return EXIT_SUCCESS;
}
