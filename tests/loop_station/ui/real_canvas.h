#pragma once
#include "definitions_cxx.hpp"
#include "gui/fonts/fonts.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class StringBuf;
namespace deluge::hid::display::oled_canvas {
enum BorderRadius : uint8_t { SMALL, BIG };
struct Point {
	int32_t x, y;
};
struct DrawLineOptions {
	bool thick = false;
	std::optional<uint8_t> min_x, max_x;
	std::optional<std::function<void(Point)>> point_callback;
};
class Canvas {
public:
	static inline bool roundedCornersEnabled = true;
	std::array<uint8_t, 128> leadingCanary{};
	uint8_t image_[OLED_MAIN_HEIGHT_PIXELS / 8][OLED_MAIN_WIDTH_PIXELS]{};
	std::array<uint8_t, 128> trailingCanary{};
	std::vector<std::string> lines;
	std::vector<int> textY;
	std::vector<int> textX, textHeight, textWidth;
	int inversions = 0, underlines = 0, pixels = 0;
	void clear() {
		std::fill(&image_[0][0], &image_[0][0] + sizeof(image_), 0);
		lines.clear();
		textY.clear();
		textX.clear();
		textHeight.clear();
		textWidth.clear();
	}
	void drawString(std::string_view text, int32_t x, int32_t y, int32_t w, int32_t h, int32_t scroll = 0,
	                int32_t end = OLED_MAIN_WIDTH_PIXELS, bool fixed = false) {
		lines.emplace_back(text);
		textY.push_back(y);
		textX.push_back(x);
		textHeight.push_back(h);
		textWidth.push_back(getStringWidthInPixels(std::string(text).c_str(), h));
		drawStringPixels(text, x, y, w, h, scroll, end, fixed);
	}
	void drawStringPixels(std::string_view, int32_t, int32_t, int32_t, int32_t, int32_t = 0,
	                      int32_t = OLED_MAIN_WIDTH_PIXELS, bool = false);
	void drawChar(uint8_t, int32_t, int32_t, int32_t, int32_t, int32_t = 0, int32_t = OLED_MAIN_WIDTH_PIXELS);
	int32_t getCharIndex(uint8_t);
	int32_t getCharWidthInPixels(uint8_t, int32_t);
	int32_t getCharSpacingInPixels(uint8_t, int32_t, bool);
	int32_t getStringWidthInPixels(const char*, int32_t);
	void drawGraphicMultiLine(const uint8_t*, int32_t, int32_t, int32_t, int32_t, int32_t, bool = false);
	void drawStringCentered(const char*, int32_t, int32_t, int32_t, int32_t, int32_t);
	void drawStringCentered(StringBuf&, int32_t, int32_t, int32_t, int32_t, int32_t);
	void drawScreenTitle(std::string_view, bool);
	void drawPixel(int32_t x, int32_t y) {
		++pixels;
		drawPixelPixels(x, y);
	}
	void drawPixelPixels(int32_t, int32_t);
	void clearPixel(int32_t, int32_t);
	void invertPixel(int32_t, int32_t);
	void drawHorizontalLine(int32_t y, int32_t x, int32_t end) {
		++underlines;
		drawHorizontalLinePixels(y, x, end);
	}
	void drawHorizontalLinePixels(int32_t, int32_t, int32_t);
	void drawLine(int32_t, int32_t, int32_t, int32_t, const DrawLineOptions& = {});
	void invertArea(int32_t, int32_t, int32_t, int32_t);
	void invertAreaRounded(int32_t x, int32_t w, int32_t y, int32_t end, BorderRadius r = SMALL) {
		++inversions;
		invertAreaRoundedPixels(x, w, y, end, r);
	}
	void invertAreaRoundedPixels(int32_t, int32_t, int32_t, int32_t, BorderRadius);
};
} // namespace deluge::hid::display::oled_canvas
