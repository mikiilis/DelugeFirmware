#pragma once

// Narrow boundaries for the extracted, unchanged production SevenSegment
// encoding/scrolling/layer code. Hardware output (PIC/OLED/sysex) is the only
// thing replaced; the encoder, layer progression and lifecycle are real.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string_view>
#include <vector>

constexpr int32_t kNumericDisplayLength = 4;
#define FLAT_CHAR 0x81u

constexpr int32_t operator""_i32(unsigned long long value) {
	return static_cast<int32_t>(value);
}

class NumericLayer {
public:
	NumericLayer() = default;
	NumericLayer(uint8_t fixedDot_) : NumericLayer() { fixedDot = fixedDot_; }
	virtual ~NumericLayer();
	virtual bool callBack() = 0;
	virtual void render(uint8_t* returnSegments) = 0;
	virtual void renderWithoutBlink(uint8_t* returnSegments);
	virtual void isNowOnTop() = 0;

	NumericLayer* next = nullptr;
	uint8_t fixedDot = 255;
};

class NumericLayerScrollingText final : public NumericLayer {
public:
	explicit NumericLayerScrollingText(uint8_t fixedDot_);
	~NumericLayerScrollingText() override;
	void render(uint8_t* returnSegments) override;
	bool callBack() override;
	void isNowOnTop() override;

	uint8_t text[256];
	uint16_t length;
	int8_t currentDirection;
	int8_t currentPos;
	int16_t initialDelay;

	int scrollsCount = -1;
};

class NumericLayerBasicText final : public NumericLayer {
public:
	NumericLayerBasicText();
	~NumericLayerBasicText() override;

	void render(uint8_t* returnSegments) override;
	void renderWithoutBlink(uint8_t* returnSegments) override;
	bool callBack() override;
	void isNowOnTop() override;

	uint8_t segments[kNumericDisplayLength];
	uint8_t blinkedSegments[kNumericDisplayLength];

	bool currentlyBlanked;

	int8_t blinkCount;

	int8_t blinkSpeed;
};

// Only used by the (unreached at runtime) transition branch of transitionToNewLayer.
class NumericLayerScrollTransition : public NumericLayer {
public:
	int8_t transitionDirection = 0;
	int32_t transitionProgress = 0;
	uint8_t segments[kNumericDisplayLength]{};
	void render(uint8_t*) override {}
	bool callBack() override { return false; }
	void isNowOnTop() override {}
};

class GeneralMemoryAllocator {
public:
	static GeneralMemoryAllocator& get() {
		static GeneralMemoryAllocator instance;
		return instance;
	}
	void* allocLowSpeed(size_t size) {
		if (onAllocate)
			onAllocate();
		if (failAllocations || failNextAllocation) {
			failNextAllocation = false;
			return nullptr;
		}
		return std::malloc(size);
	}
	bool failAllocations = false, failNextAllocation = false;
	std::function<void()> onAllocate;
};
inline void delugeDealloc(void* pointer) {
	std::free(pointer);
}

enum class TimerName { DISPLAY, LED_BLINK };
struct UiTimerManager {
	void setTimer(TimerName, uint32_t) {}
	void unsetTimer(TimerName) {}
	void setTimerByOtherTimer(TimerName, TimerName) {}
	bool isTimerSet(TimerName) { return false; }
};
inline UiTimerManager uiTimerManager;

constexpr int32_t kFlashTime = 80;
constexpr int32_t kInitialFlashTime = 160;
namespace indicator_leds {
inline bool ledBlinkState[1] = {false};
}

namespace deluge::hid::display {

class SevenSegment {
public:
	// The rendered hardware output boundary; the probe records the last frame.
	void render() {
		if (topLayer) {
			topLayer->render(lastDisplay_.data());
		}
	}

	int32_t encodeText(std::string_view newText, uint8_t* destination, bool alignRight,
	                   const std::vector<uint8_t>& dotPositions = {}, bool limitToDisplayLength = true,
	                   int32_t scrollPos = 0) const;
	NumericLayerScrollingText* setScrollingText(char const* newText, int32_t startAtTextPos = 0,
	                                            int32_t initialDelay = 600, int count = -1, uint8_t fixedDot = 255);
	NumericLayerScrollingText* installScrollingOverlay(char const* newText, int32_t startAtTextPos = 0,
	                                                   int32_t initialDelay = 600, int count = -1,
	                                                   uint8_t fixedDot = 255);
	NumericLayerScrollingText* setScrollingTextInternal(char const* newText, int32_t startAtTextPos,
	                                                    int32_t initialDelay, int count, uint8_t fixedDot,
	                                                    bool asOverlay);
	int32_t getEncodedPosFromLeft(int32_t textPos, char const* text, bool* andAHalf);
	void setTopLayer(NumericLayer* newTopLayer);
	void deleteAllLayers();
	void removeTopLayer();
	bool removeLayer(NumericLayer* expected);
	void replaceBottomLayer(NumericLayer* newLayer);
	void transitionToNewLayer(NumericLayer* newLayer);
	void setTextWithMultipleDots(std::string_view newText, std::vector<uint8_t> dotPositions, bool alignRight,
	                             bool doBlink, uint8_t* newBlinkMask, bool blinkImmediately);
	void setText(std::string_view newText, bool alignRight = false, uint8_t drawDot = 255, bool doBlink = false,
	             uint8_t* newBlinkMask = nullptr, bool blinkImmediately = false, bool shouldBlinkFast = false,
	             int32_t scrollPos = 0, uint8_t* encodedAddition = nullptr, bool justReplaceBottomLayer = false);
	void innerSetText(std::string_view newText, bool alignRight = false, std::vector<uint8_t> dotPositions = {},
	                  bool doBlink = false, uint8_t* newBlinkMask = nullptr, bool blinkImmediately = false,
	                  bool shouldBlinkFast = false, int32_t scrollPos = 0, uint8_t* encodedAddition = nullptr,
	                  bool justReplaceBottomLayer = false);

	NumericLayer* topLayer = nullptr;
	bool popupActive = false;
	int8_t nextTransitionDirection = 0;
	std::array<uint8_t, kNumericDisplayLength> lastDisplay_{};
	bool use_lowercase = false;
};

} // namespace deluge::hid::display
