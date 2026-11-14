#pragma once

#include "config.h"
#include <array>

namespace loop_station {

enum class MenuScope : uint8_t { Global, Track };
enum class MenuField : uint8_t {
	RecordAction,
	StartMode,
	StartInterval,
	StopMode,
	StopInterval,
	Source,
	Monitoring,
	Duration,
	Playback,
	Transpose,
	Colour,
	CopyToTrack
};

struct MenuFields {
	std::array<MenuField, 12> rows{};
	uint8_t count = 0;
	constexpr bool contains(MenuField field) const {
		for (uint8_t i = 0; i < count; ++i)
			if (rows[i] == field)
				return true;
		return false;
	}
};

MenuFields visibleMenuFields(MenuScope scope, const Settings& settings);
constexpr bool shouldCloseSettingsMenu(bool backPressed, bool sessionViewPressed, bool shiftPressed) {
	return backPressed || (sessionViewPressed && !shiftPressed);
}
uint16_t maxDurationBars(MemoryProfile profile, uint32_t framesPerBar);
uint16_t maxDurationBarsForPages(uint32_t freePages, uint32_t framesPerBar);
int16_t clampTransposeCents(int32_t cents);
int16_t stepTransposeCents(int16_t currentCents, int32_t encoderOffset, bool fine = false);
inline constexpr std::array<Colour, 11> kTrackColours = {
    Colour::Gold, Colour::Yellow, Colour::Lime,    Colour::Turquoise, Colour::Cyan, Colour::Sky,
    Colour::Blue, Colour::Purple, Colour::Magenta, Colour::Pink,      Colour::White};
inline constexpr size_t kTrackColourCount = kTrackColours.size();
constexpr Colour trackColourAt(unsigned index) {
	return index < kTrackColours.size() ? kTrackColours[index] : Colour::Yellow;
}

} // namespace loop_station
