/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui/menu_item/loop_station/settings.h"

#include "definitions.h"
#include "gui/menu_item/audio_clip/attack.h"
#include "gui/menu_item/decimal.h"
#include "gui/menu_item/horizontal_menu.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/selection.h"
#include "gui/menu_item/toggle.h"
#include "gui/menu_item/value_scaling.h"
#include "gui/menu_item/voice/priority.h"
#include "gui/ui/menus.h"
#include "gui/views/loop_station_view.h"
#include "model/song/song.h"

#include <algorithm>
#include <array>
#include <limits>

namespace deluge::gui::menu_item::loop_station {
namespace {
using namespace deluge::loop_station;

SlotId activeSlot = SlotId::ONE;

SongModel& model() {
	return currentSong->loopStation;
}

class GlobalSelection final : public Selection {
public:
	GlobalSelection(l10n::String name, GlobalParameter parameter) : Selection(name), parameter_(parameter) {}

	void readCurrentValue() override { setValue(readGlobalParameter(model(), parameter_)); }
	void writeCurrentValue() override { writeGlobalParameter(model(), parameter_, getValue()); }
	deluge::vector<std::string_view> getOptions(OptType) override {
		switch (parameter_) {
		case GlobalParameter::RECORD_ACTION:
			return {"REC > DUB > PLAY", "REC > PLAY > DUB"};
		case GlobalParameter::START_ACTION:
		case GlobalParameter::STOP_ACTION:
			return {"Immediate", "Synced"};
		case GlobalParameter::START_INTERVAL:
		case GlobalParameter::STOP_INTERVAL:
			return {"1 bar",           "1/2 bar",         "1/4 bar",          "1/8 bar",
			        "1/16 bar",        "1/32 bar",        "1 bar triplet",    "1/2 bar triplet",
			        "1/4 bar triplet", "1/8 bar triplet", "1/16 bar triplet", "1/32 bar triplet"};
		case GlobalParameter::DIM_BORDER_COLOUR:
			return {"Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Purple", "Magenta", "White"};
		}
		return {};
	}

	bool isRelevant(ModControllableAudio*, int32_t) override {
		if (parameter_ == GlobalParameter::START_INTERVAL)
			return model().global.startAction.mode == ActionMode::SYNCED;
		if (parameter_ == GlobalParameter::STOP_INTERVAL)
			return model().global.stopAction.mode == ActionMode::SYNCED;
		return true;
	}

private:
	GlobalParameter parameter_;
};

class TrackSelection final : public Selection {
public:
	TrackSelection(l10n::String name, TrackParameter parameter) : Selection(name), parameter_(parameter) {}

	void readCurrentValue() override {
		if (parameter_ == TrackParameter::PROGRESS_COLOUR) {
			auto colour = model().tracks[static_cast<size_t>(activeSlot)].settings.progressColour;
			auto found = std::find(kProgressColours.begin(), kProgressColours.end(), colour);
			setValue(found == kProgressColours.end() ? 0 : std::distance(kProgressColours.begin(), found));
		}
		else {
			setValue(readTrackParameter(model(), activeSlot, parameter_));
		}
	}
	void writeCurrentValue() override {
		if (parameter_ == TrackParameter::PROGRESS_COLOUR) {
			auto index = std::clamp<int32_t>(getValue(), 0, static_cast<int32_t>(kProgressColours.size() - 1));
			writeTrackParameter(model(), activeSlot, parameter_, static_cast<int32_t>(kProgressColours[index]));
		}
		else {
			writeTrackParameter(model(), activeSlot, parameter_, getValue());
		}
		if (parameter_ == TrackParameter::SOURCE) {
			loopStationView.applyTrackSource(activeSlot);
		}
	}
	deluge::vector<std::string_view> getOptions(OptType) override {
		switch (parameter_) {
		case TrackParameter::SOURCE:
			return {"Left", "Right", "Stereo", "Balanced"};
		case TrackParameter::PROGRESS_COLOUR:
			return {"Gold", "Yellow", "Lime", "Turquoise", "Cyan", "Sky", "Blue", "Purple", "Magenta", "Pink", "White"};
		case TrackParameter::PLAYBACK:
			return {"Loop", "One Shot"};
		default:
			return {};
		}
	}

private:
	TrackParameter parameter_;
};

class TrackToggle final : public Toggle {
public:
	TrackToggle(l10n::String name, TrackParameter parameter) : Toggle(name), parameter_(parameter) {}

	void readCurrentValue() override { setValue(readTrackParameter(model(), activeSlot, parameter_) != 0); }
	void writeCurrentValue() override {
		writeTrackParameter(model(), activeSlot, parameter_, getValue());
		if (parameter_ == TrackParameter::MONITORING) {
			loopStationView.refreshMonitoring();
		}
	}

private:
	TrackParameter parameter_;
};

class SourceTrack final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		setValue(readTrackParameter(model(), activeSlot, TrackParameter::SOURCE_TRACK));
	}
	void writeCurrentValue() override {
		writeTrackParameter(model(), activeSlot, TrackParameter::SOURCE_TRACK, getValue());
		loopStationView.applyTrackSource(activeSlot);
	}
	bool isRelevant(ModControllableAudio*, int32_t) override {
		return model().tracks[static_cast<size_t>(activeSlot)].settings.source == SourceType::SONG_TRACK;
	}
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override {
		return std::max<int32_t>(0, currentSong->getNumOutputs() - 1);
	}
	int32_t getDisplayValue() override { return getValue() + 1; }
};

class Duration final : public IntegerWithOff {
public:
	using IntegerWithOff::IntegerWithOff;

	void readCurrentValue() override {
		setValue(readTrackParameter(model(), activeSlot, TrackParameter::DURATION_BARS));
	}
	void writeCurrentValue() override {
		writeTrackParameter(model(), activeSlot, TrackParameter::DURATION_BARS, getValue());
	}
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override {
		uint32_t barLength = std::max<uint32_t>(1, currentSong->getBarLength());
		return std::min<uint32_t>(std::numeric_limits<uint16_t>::max(), kMaxLoopSequenceLength / barLength);
	}
	const char* getUnit() override { return " bars"; }
};

class TrackTranspose final : public Decimal {
public:
	using Decimal::Decimal;

	void readCurrentValue() override {
		auto const& settings = model().tracks[static_cast<size_t>(activeSlot)].settings;
		this->setValue(computeCurrentValueForTranspose(settings.transpose, settings.transposeCents));
	}
	void writeCurrentValue() override {
		int32_t transpose, cents;
		computeFinalValuesForTranspose(this->getValue(), &transpose, &cents);

		auto& settings = model().tracks[static_cast<size_t>(activeSlot)].settings;
		settings.transpose = static_cast<int16_t>(transpose);
		settings.transposeCents = static_cast<int8_t>(cents);
		writeTrackParameter(model(), activeSlot, TrackParameter::TRANSPOSE, this->getValue());

		// Apply immediately to live monitoring if active
		if (loopStationView.hasActiveEngine()) {
			loopStationView.refreshMonitoring();
		}
	}

	[[nodiscard]] int32_t getMinValue() const override { return -2450; }
	[[nodiscard]] int32_t getMaxValue() const override { return 2450; }
	[[nodiscard]] int32_t getNumDecimalPlaces() const override { return 2; }
	const char* getUnit() { return "st"; }
};

class CopyToTrack final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override { setValue(0); }
	void writeCurrentValue() override {
		// Rotating chooses a destination. Copying happens only on confirmation.
	}
	deluge::vector<std::string_view> getOptions(OptType) override {
		static constexpr std::array<std::string_view, kTrackCount> names{"Track 1", "Track 2", "Track 3", "Track 4",
		                                                                 "Track 5", "Track 6", "Track 7", "Track 8"};
		deluge::vector<std::string_view> options;
		options.reserve(kTrackCount - 1);
		for (size_t i = 0; i < names.size(); ++i) {
			if (i != static_cast<size_t>(activeSlot)) {
				options.push_back(names[i]);
			}
		}
		return options;
	}
	MenuItem* selectButtonPress() override {
		auto sourceIndex = static_cast<size_t>(activeSlot);
		auto destinationIndex = static_cast<size_t>(std::clamp<int32_t>(getValue(), 0, kTrackCount - 2));
		if (destinationIndex >= sourceIndex) {
			++destinationIndex;
		}
		bool copied = loopStationView.copyTrackConfiguration(activeSlot, static_cast<SlotId>(destinationIndex));
		display->displayPopup(l10n::get(copied ? l10n::String::STRING_FOR_LOOP_STATION_CONFIGURATION_COPIED
		                                       : l10n::String::STRING_FOR_LOOP_STATION_TARGET_BUSY));
		return nullptr;
	}
};
} // namespace

PLACE_SDRAM_BSS GlobalSelection recordAction{l10n::String::STRING_FOR_LOOP_STATION_RECORD_ACTION,
                                             GlobalParameter::RECORD_ACTION};
PLACE_SDRAM_BSS GlobalSelection startMode{l10n::String::STRING_FOR_MODE, GlobalParameter::START_ACTION};
PLACE_SDRAM_BSS GlobalSelection stopMode{l10n::String::STRING_FOR_MODE, GlobalParameter::STOP_ACTION};
PLACE_SDRAM_BSS GlobalSelection startInterval{l10n::String::STRING_FOR_LOOP_STATION_INTERVAL,
                                              GlobalParameter::START_INTERVAL};
PLACE_SDRAM_BSS GlobalSelection stopInterval{l10n::String::STRING_FOR_LOOP_STATION_INTERVAL,
                                             GlobalParameter::STOP_INTERVAL};
PLACE_SDRAM_BSS Submenu startAction{l10n::String::STRING_FOR_LOOP_STATION_START_ACTION, {&startMode, &startInterval}};
PLACE_SDRAM_BSS Submenu stopAction{l10n::String::STRING_FOR_LOOP_STATION_STOP_ACTION, {&stopMode, &stopInterval}};
PLACE_SDRAM_BSS GlobalSelection dimBorderColour{l10n::String::STRING_FOR_LOOP_STATION_DIM_BORDER_COLOUR,
                                                GlobalParameter::DIM_BORDER_COLOUR};

PLACE_SDRAM_BSS TrackSelection source{l10n::String::STRING_FOR_AUDIO_SOURCE, TrackParameter::SOURCE};
PLACE_SDRAM_BSS TrackToggle monitoring{l10n::String::STRING_FOR_MONITORING, TrackParameter::MONITORING};
PLACE_SDRAM_BSS Duration duration{l10n::String::STRING_FOR_LOOP_STATION_RECORD_DURATION};
PLACE_SDRAM_BSS TrackSelection progressColour{l10n::String::STRING_FOR_LOOP_STATION_PROGRESS_COLOUR,
                                              TrackParameter::PROGRESS_COLOUR};
PLACE_SDRAM_BSS TrackSelection playback{l10n::String::STRING_FOR_LOOP_STATION_PLAYBACK, TrackParameter::PLAYBACK};
PLACE_SDRAM_BSS TrackTranspose transpose{l10n::String::STRING_FOR_LOOP_STATION_TRANSPOSE};
PLACE_SDRAM_BSS CopyToTrack copyToTrack{l10n::String::STRING_FOR_LOOP_STATION_COPY_TO_TRACK};

PLACE_SDRAM_BSS Submenu globalSettingsMenu{l10n::String::STRING_FOR_LOOP_STATION,
                                           {&recordAction, &startAction, &stopAction}};

PLACE_SDRAM_BSS Submenu trackSettingsMenu{l10n::String::STRING_FOR_LOOP_STATION_TRACK,
                                          {&source, &monitoring, &duration, &progressColour, &playback, &transpose,
                                           &copyToTrack, &audioClipActionsMenu, &audioClipMasterMenu, &audioCompMenu,
                                           &globalFiltersMenu, &audioClipFXMenu, &globalSidechainMenu,
                                           &audioClipSampleMenu, &audioClipAttackMenu, &priorityMenu}};

void selectTrack(SlotId slot) {
	activeSlot = slot;
}

SlotId selectedTrack() {
	return activeSlot;
}

} // namespace deluge::gui::menu_item::loop_station
