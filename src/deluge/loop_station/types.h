#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace loop_station {

using SlotId = uint8_t;
using FrameIndex = uint32_t;
using SampleClock = uint64_t;
using Generation = uint64_t;

inline constexpr SlotId kTrackCount = 8;
inline constexpr SlotId kInvalidSlot = 0xff;

struct Frame {
	int32_t l;
	int32_t r;
};
static_assert(sizeof(Frame) == 8);

enum class Status : uint8_t {
	Ok,
	Invalid,
	Busy,
	NoMemory,
	SourceUnavailable,
	Stale,
	Unavailable,
	StorageError,
	Cancelled
};

struct PhraseHandle {
	SlotId slot = kInvalidSlot;
	Generation generation = 0;
	constexpr bool valid() const { return slot < kTrackCount && generation != 0; }
};

struct SnapshotId {
	Generation generation = 0;
	constexpr bool valid() const { return generation != 0; }
};

struct RestoreId {
	Generation generation = 0;
	constexpr bool valid() const { return generation != 0; }
};

enum class RecordAction : uint8_t { REC_PLAY_DUB, REC_DUB_PLAY };
enum class ActionMode : uint8_t { Immediate = 0, Synced = 1 };

// XML IDs are persistent. Menu order is defined separately below.
enum class Interval : uint8_t {
	Bar = 0,
	Half = 1,
	Quarter = 2,
	Eighth = 3,
	BarTriplet = 4,
	HalfTriplet = 5,
	QuarterTriplet = 6,
	EighthTriplet = 7,
	Sixteenth = 8,
	ThirtySecond = 9,
	SixteenthTriplet = 10,
	ThirtySecondTriplet = 11
};

inline constexpr std::array<Interval, 12> kIntervalMenuOrder = {Interval::Bar,
                                                                Interval::Half,
                                                                Interval::Quarter,
                                                                Interval::Eighth,
                                                                Interval::Sixteenth,
                                                                Interval::ThirtySecond,
                                                                Interval::BarTriplet,
                                                                Interval::HalfTriplet,
                                                                Interval::QuarterTriplet,
                                                                Interval::EighthTriplet,
                                                                Interval::SixteenthTriplet,
                                                                Interval::ThirtySecondTriplet};

constexpr Interval intervalAtMenuIndex(unsigned index) {
	return index < kIntervalMenuOrder.size() ? kIntervalMenuOrder[index] : Interval::Sixteenth;
}

constexpr unsigned menuIndexOfInterval(Interval interval) {
	for (unsigned i = 0; i < kIntervalMenuOrder.size(); ++i) {
		if (kIntervalMenuOrder[i] == interval)
			return i;
	}
	return 4; // Invalid IDs use the default straight sixteenth.
}

struct ActionSettings {
	ActionMode mode = ActionMode::Synced;
	Interval interval = Interval::Sixteenth;
};

enum class Source : uint8_t { Left, Right, Stereo, Balanced };
enum class Monitoring : uint8_t { Off, On };
enum class PlaybackMode : uint8_t { Loop, OneShot };
enum class Colour : uint8_t { Gold, Yellow, Lime, Turquoise, Cyan, Sky, Blue, Purple, Magenta, Pink, White };

struct TrackSettings {
	Source source = Source::Left;
	Monitoring monitoring = Monitoring::On;
	uint16_t recordDurationBars = 0; // Zero selects manual duration.
	PlaybackMode playback = PlaybackMode::Loop;
	Colour colour = Colour::Yellow;
	int16_t transposeCents = 0; // Hundredths of a semitone, -2450 to +2450.
};

struct Settings {
	RecordAction recordAction = RecordAction::REC_PLAY_DUB;
	ActionSettings startAction{};
	ActionSettings stopAction{};
	std::array<TrackSettings, kTrackCount> tracks{};
};

constexpr bool validSlot(SlotId slot) {
	return slot < kTrackCount;
}

constexpr TrackSettings defaultTrackSettings(SlotId slot) {
	constexpr std::array<Colour, kTrackCount> colours = {Colour::Yellow,  Colour::Cyan, Colour::Blue, Colour::Purple,
	                                                     Colour::Magenta, Colour::Pink, Colour::Gold, Colour::Lime};
	TrackSettings settings{};
	if (validSlot(slot))
		settings.colour = colours[slot];
	return settings;
}

constexpr Settings defaultSettings() {
	Settings settings{};
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		settings.tracks[slot] = defaultTrackSettings(slot);
	}
	return settings;
}

} // namespace loop_station
