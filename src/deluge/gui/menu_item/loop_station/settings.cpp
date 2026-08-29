/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui/menu_item/loop_station/settings.h"

#include "definitions.h"
#include "gui/menu_item/value_scaling.h"
#include "gui/menu_item/audio_clip/attack.h"
#include "gui/menu_item/horizontal_menu.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/selection.h"
#include "gui/menu_item/toggle.h"
#include "gui/menu_item/voice/priority.h"
#include "gui/ui/menus.h"
#include "gui/views/loop_station_view.h"
#include "model/song/song.h"

#include <algorithm>
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
		case GlobalParameter::QUANTIZE:
			return {"Off", "Smart", "Measure"};
		case GlobalParameter::DIM_BORDER_COLOUR:
			return {"Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Purple", "Magenta", "White"};
		}
		return {};
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
			return {"Left", "Right", "Stereo", "Balanced", "Mix Pre-FX", "Mix Post-FX", "Song Track"};
		case TrackParameter::PROGRESS_COLOUR:
			return {"Yellow", "Cyan", "Blue", "Purple", "Magenta", "White"};
		case TrackParameter::PLAYBACK:
			return {"Loop", "One Shot"};
		case TrackParameter::START_MODE:
			return {"Immediate", "Fade"};
		case TrackParameter::STOP_MODE:
			return {"Loop End", "Immediate", "Fade"};
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

class FadeTime final : public Integer {
public:
	using Integer::Integer;

	void readCurrentValue() override {
		setValue(readTrackParameter(model(), activeSlot, TrackParameter::FADE_TIME_BARS));
	}
	void writeCurrentValue() override {
		writeTrackParameter(model(), activeSlot, TrackParameter::FADE_TIME_BARS, getValue());
	}
	bool isRelevant(ModControllableAudio*, int32_t) override {
		auto const& settings = model().tracks[static_cast<size_t>(activeSlot)].settings;
		return settings.startMode == StartMode::FADE || settings.stopMode == StopMode::FADE;
	}
	[[nodiscard]] int32_t getMinValue() const override { return 1; }
	[[nodiscard]] int32_t getMaxValue() const override { return 64; }
	const char* getUnit() override { return " bars"; }
};

PLACE_SDRAM_BSS GlobalSelection recordAction{l10n::String::STRING_FOR_LOOP_STATION_RECORD_ACTION,
                                             GlobalParameter::RECORD_ACTION};
PLACE_SDRAM_BSS GlobalSelection recordQuantize{l10n::String::STRING_FOR_LOOP_STATION_RECORD_QUANTIZE,
                                               GlobalParameter::QUANTIZE};
PLACE_SDRAM_BSS GlobalSelection dimBorderColour{l10n::String::STRING_FOR_LOOP_STATION_DIM_BORDER_COLOUR,
                                                GlobalParameter::DIM_BORDER_COLOUR};

PLACE_SDRAM_BSS TrackSelection source{l10n::String::STRING_FOR_AUDIO_SOURCE, TrackParameter::SOURCE};
PLACE_SDRAM_BSS SourceTrack sourceTrack{l10n::String::STRING_FOR_LOOP_STATION_SOURCE_TRACK};
PLACE_SDRAM_BSS TrackToggle monitoring{l10n::String::STRING_FOR_MONITORING, TrackParameter::MONITORING};
PLACE_SDRAM_BSS Duration duration{l10n::String::STRING_FOR_LOOP_STATION_RECORD_DURATION};
PLACE_SDRAM_BSS TrackSelection progressColour{l10n::String::STRING_FOR_LOOP_STATION_PROGRESS_COLOUR,
                                              TrackParameter::PROGRESS_COLOUR};
PLACE_SDRAM_BSS TrackSelection playback{l10n::String::STRING_FOR_LOOP_STATION_PLAYBACK, TrackParameter::PLAYBACK};
PLACE_SDRAM_BSS TrackToggle loopSync{l10n::String::STRING_FOR_LOOP_STATION_LOOP_SYNC, TrackParameter::LOOP_SYNC};
PLACE_SDRAM_BSS TrackSelection startMode{l10n::String::STRING_FOR_LOOP_STATION_START_MODE, TrackParameter::START_MODE};
PLACE_SDRAM_BSS TrackSelection stopMode{l10n::String::STRING_FOR_LOOP_STATION_STOP_MODE, TrackParameter::STOP_MODE};
PLACE_SDRAM_BSS FadeTime fadeTime{l10n::String::STRING_FOR_LOOP_STATION_FADE_TIME};
} // namespace

PLACE_SDRAM_BSS Submenu globalSettingsMenu{l10n::String::STRING_FOR_LOOP_STATION,
                                           {&recordAction, &recordQuantize, &dimBorderColour}};

PLACE_SDRAM_BSS Submenu trackSettingsMenu{l10n::String::STRING_FOR_LOOP_STATION_TRACK,
	                                      {&source, &sourceTrack, &monitoring, &duration, &progressColour, &playback,
	                                       &loopSync, &startMode, &stopMode, &fadeTime, &audioClipActionsMenu,
	                                       &audioClipMasterMenu, &audioCompMenu, &globalFiltersMenu, &audioClipFXMenu,
	                                       &globalSidechainMenu, &audioClipSampleMenu, &audioClipAttackMenu,
	                                       &priorityMenu}};

void selectTrack(SlotId slot) {
	activeSlot = slot;
}

SlotId selectedTrack() {
	return activeSlot;
}

} // namespace deluge::gui::menu_item::loop_station
