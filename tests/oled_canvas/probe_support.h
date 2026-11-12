#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "gui/fonts/fonts.h"

constexpr int32_t OLED_MAIN_WIDTH_PIXELS = 128;
constexpr int32_t OLED_MAIN_HEIGHT_PIXELS = 48;
constexpr int32_t kTextSpacingX = 6;

namespace deluge::hid::display::oled_canvas {
class Canvas {
public:
	static constexpr int32_t kImageHeight = OLED_MAIN_HEIGHT_PIXELS >> 3;
	static constexpr int32_t kImageWidth = OLED_MAIN_WIDTH_PIXELS;
	using ImageStore = uint8_t[kImageHeight][kImageWidth];

	std::array<uint8_t, 128> leadingCanary{};
	ImageStore image_{};
	std::array<uint8_t, 128> trailingCanary{};

	void clear() { std::fill(&image_[0][0], &image_[0][0] + kImageHeight * kImageWidth, 0); }
	ImageStore& hackGetImageStore() { return image_; }

	void drawString(std::string_view, int32_t, int32_t, int32_t, int32_t, int32_t = 0, int32_t = 128, bool = false);
	void drawChar(uint8_t, int32_t, int32_t, int32_t, int32_t, int32_t = 0, int32_t = 128);
	int32_t getCharIndex(uint8_t);
	int32_t getCharWidthInPixels(uint8_t, int32_t);
	int32_t getCharSpacingInPixels(uint8_t, int32_t, bool);
	int32_t getStringWidthInPixels(char const*, int32_t);
	void drawGraphicMultiLine(uint8_t const*, int32_t, int32_t, int32_t, int32_t, int32_t, bool = false);
};
} // namespace deluge::hid::display::oled_canvas

using deluge::hid::display::oled_canvas::Canvas;
