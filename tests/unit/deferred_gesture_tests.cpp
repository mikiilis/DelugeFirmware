#include "CppUTest/TestHarness.h"
#include "loop_station/deferred_gesture.h"

using namespace deluge::loop_station;

TEST_GROUP(DeferredLoopStationGesture){};

TEST(DeferredLoopStationGesture, ShortPressRetainsItsOriginalSamples) {
	DeferredGlobalGestureQueue gestures;
	CHECK_TRUE(gestures.press(DeferredGlobalButton::SAVE, 1000, false));
	CHECK_TRUE(gestures.release(DeferredGlobalButton::SAVE, 10999));
	auto gesture = gestures.take();
	CHECK(gesture.has_value());
	CHECK_EQUAL(1000, gesture->pressSample);
	CHECK(gesture->releaseSample.has_value());
	CHECK_EQUAL(10999, *gesture->releaseSample);
	CHECK(gesture->classify(10000) == DeferredGestureKind::SHORT_PRESS);
}

TEST(DeferredLoopStationGesture, LongHoldRemainsLongAcrossSampleCounterWrap) {
	DeferredGlobalGestureQueue gestures;
	CHECK_TRUE(gestures.press(DeferredGlobalButton::LOAD, UINT32_MAX - 1000, false));
	CHECK_TRUE(gestures.release(DeferredGlobalButton::LOAD, 22000));
	auto gesture = gestures.take();
	CHECK(gesture->classify(10000) == DeferredGestureKind::LONG_PRESS);
}

TEST(DeferredLoopStationGesture, HeldThroughDrainDispatchesPressOnlyAndReleaseIsNotReplayed) {
	DeferredGlobalGestureQueue gestures;
	CHECK_TRUE(gestures.press(DeferredGlobalButton::SAVE, 400, false));
	auto gesture = gestures.take();
	CHECK(gesture->classify(10000) == DeferredGestureKind::STILL_HELD);
	CHECK_FALSE(gestures.release(DeferredGlobalButton::SAVE, 12000));
	CHECK_FALSE(gestures.take().has_value());
}

TEST(DeferredLoopStationGesture, ShiftAtPressIsStableWhenShiftChangesDuringDrain) {
	DeferredGlobalGestureQueue gestures;
	CHECK_TRUE(gestures.press(DeferredGlobalButton::LOAD, 500, true));
	CHECK_TRUE(gestures.release(DeferredGlobalButton::LOAD, 600));
	auto gesture = gestures.take();
	bool shiftNow = false;
	CHECK_FALSE(shiftNow);
	CHECK_TRUE(gesture->shiftHeld);
	CHECK(gesture->classify(10000) == DeferredGestureKind::CLEAR_SONG);
}

TEST(DeferredLoopStationGesture, ShiftLoadRetainsClearSongIntent) {
	DeferredGlobalGestureQueue gestures;
	CHECK_TRUE(gestures.press(DeferredGlobalButton::LOAD, 750, true));
	auto gesture = gestures.take();
	CHECK(gesture->classify(10000) == DeferredGestureKind::CLEAR_SONG);
}

TEST(DeferredLoopStationGesture, FirstRequestWinsAndDrainCompletionDispatchesOnlyOnce) {
	DeferredGlobalGestureQueue gestures;
	CHECK_TRUE(gestures.press(DeferredGlobalButton::SAVE, 800, false));
	CHECK_FALSE(gestures.press(DeferredGlobalButton::LOAD, 900, true));
	CHECK_FALSE(gestures.release(DeferredGlobalButton::LOAD, 1000));
	CHECK_TRUE(gestures.release(DeferredGlobalButton::SAVE, 1100));
	auto gesture = gestures.take();
	CHECK(gesture->button == DeferredGlobalButton::SAVE);
	CHECK_FALSE(gestures.take().has_value());
}
