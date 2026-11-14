#include "settings_items.h"

#include "deluge/loop_station/song_model.h"
#include "extern.h"
#include "gui/menu_item/horizontal_menu.h"
#include "hid/buttons.h"
#include "model/song/song.h"
#include <algorithm>
#include <cassert>

namespace deluge::gui::menu_item::loop_station {
using l10n::String;
using ls::MenuField;

ls::SongModel* SettingsContext::model() const {
	return currentSong ? currentSong->loopStation() : nullptr;
}
bool SettingsContext::relevant(MenuField field) const {
	return model() && (scope != ls::MenuScope::Track || ls::validSlot(slot))
	       && ls::visibleMenuFields(scope, model()->engine().settings()).contains(field);
}
int32_t SettingsContext::read(MenuField field) const {
	auto* song = model();
	if (!song)
		return 0;
	const auto& settings = song->engine().settings();
	switch (field) {
	case MenuField::RecordAction:
		return int32_t(settings.recordAction);
	case MenuField::StartMode:
		return int32_t(settings.startAction.mode);
	case MenuField::StartInterval:
		return ls::menuIndexOfInterval(settings.startAction.interval);
	case MenuField::StopMode:
		return int32_t(settings.stopAction.mode);
	case MenuField::StopInterval:
		return ls::menuIndexOfInterval(settings.stopAction.interval);
	default:
		break;
	}
	if (!ls::validSlot(slot))
		return 0;
	const auto& track = settings.tracks[slot];
	switch (field) {
	case MenuField::Source:
		return int32_t(track.source);
	case MenuField::Monitoring:
		return int32_t(track.monitoring);
	case MenuField::Duration:
		return track.recordDurationBars;
	case MenuField::Playback:
		return int32_t(track.playback);
	case MenuField::Transpose:
		return track.transposeCents;
	case MenuField::Colour:
		return std::find(ls::kTrackColours.begin(), ls::kTrackColours.end(), track.colour) - ls::kTrackColours.begin();
	default:
		return 0;
	}
}
ls::Status SettingsContext::write(MenuField field, int32_t value) {
	auto* song = model();
	if (!song)
		return ls::Status::Unavailable;
	const bool global = field <= MenuField::StopInterval;
	if (global != (scope == ls::MenuScope::Global) || (!global && !ls::validSlot(slot)))
		return ls::Status::Invalid;
	int32_t maximum;
	switch (field) {
	case MenuField::RecordAction:
	case MenuField::StartMode:
	case MenuField::StopMode:
	case MenuField::Monitoring:
	case MenuField::Playback:
		maximum = 1;
		break;
	case MenuField::StartInterval:
	case MenuField::StopInterval:
		maximum = ls::kIntervalMenuOrder.size() - 1;
		break;
	case MenuField::Source:
		maximum = 3;
		break;
	case MenuField::Duration:
		maximum = maxDurationBars();
		break;
	case MenuField::Transpose:
		maximum = 2450;
		break;
	case MenuField::Colour:
		maximum = ls::kTrackColourCount - 1;
		break;
	default:
		return ls::Status::Invalid; // Copy is never a scalar setting write.
	}
	if (value < (field == MenuField::Transpose ? -2450 : 0) || value > maximum)
		return ls::Status::Invalid;
	// Copy immediately before changing one field: never replay a stale menu snapshot.
	auto settings = song->engine().settings();
	switch (field) {
	case MenuField::RecordAction:
		settings.recordAction = static_cast<ls::RecordAction>(value);
		break;
	case MenuField::StartMode:
		settings.startAction.mode = static_cast<ls::ActionMode>(value);
		break;
	case MenuField::StartInterval:
		settings.startAction.interval = ls::intervalAtMenuIndex(value);
		break;
	case MenuField::StopMode:
		settings.stopAction.mode = static_cast<ls::ActionMode>(value);
		break;
	case MenuField::StopInterval:
		settings.stopAction.interval = ls::intervalAtMenuIndex(value);
		break;
	case MenuField::Source:
		settings.tracks[slot].source = static_cast<ls::Source>(value);
		break;
	case MenuField::Monitoring:
		settings.tracks[slot].monitoring = static_cast<ls::Monitoring>(value);
		break;
	case MenuField::Duration:
		settings.tracks[slot].recordDurationBars = value;
		break;
	case MenuField::Playback:
		settings.tracks[slot].playback = static_cast<ls::PlaybackMode>(value);
		break;
	case MenuField::Transpose:
		settings.tracks[slot].transposeCents = value;
		break;
	case MenuField::Colour:
		settings.tracks[slot].colour = ls::trackColourAt(value);
		break;
	default:
		return ls::Status::Invalid;
	}
	return song->dispatch(ls::Command::configure(settings));
}
uint16_t SettingsContext::maxDurationBars() const {
	auto* song = model();
	return song && ls::validSlot(slot) ? song->maxDurationBarsForTrack(slot, song->clock().samplesForBars(1)) : 0;
}

deluge::vector<std::string_view> EnumSetting::getOptions(OptType) {
	switch (field_) {
	case MenuField::RecordAction:
		return {"REC PLAY DUB", "REC DUB PLAY"};
	case MenuField::StartMode:
	case MenuField::StopMode:
		return {"Immediate", "Synced"};
	case MenuField::StartInterval:
	case MenuField::StopInterval:
		return {"Bar",         "1/2",         "1/4",         "1/8",         "1/16",         "1/32",
		        "Bar triplet", "1/2 triplet", "1/4 triplet", "1/8 triplet", "1/16 triplet", "1/32 triplet"};
	case MenuField::Source:
		return {"Left", "Right", "Stereo", "Balanced"};
	case MenuField::Monitoring:
		return {"Off", "On"};
	case MenuField::Playback:
		return {"Loop", "One Shot"};
	case MenuField::Colour:
		return {"Gold", "Yellow", "Lime", "Turquoise", "Cyan", "Sky", "Blue", "Purple", "Magenta", "Pink", "White"};
	default:
		return {};
	}
}
void EnumSetting::writeCurrentValue() {
	context_.write(field_, getValue());
	readCurrentValue();
}
void EnumSetting::selectEncoderAction(int32_t offset) {
	readCurrentValue();
	const int64_t count = getOptions().size();
	const int64_t next = int64_t(getValue()) + offset;
	setValue((next % count + count) % count);
	Value<int32_t>::selectEncoderAction(offset);
}
void EnumSetting::getNotificationValue(StringBuf& value) {
	value.append(getOptions()[getValue()]);
}

DurationSetting::DurationSetting(SettingsContext& context)
    : Integer(String::STRING_FOR_LOOP_STATION_DURATION), context_(context) {
}
void DurationSetting::writeCurrentValue() {
	context_.write(MenuField::Duration, getValue());
	readCurrentValue();
}
void DurationSetting::selectEncoderAction(int32_t offset) {
	readCurrentValue();
	setValue(std::clamp<int64_t>(int64_t(getValue()) + offset, 0, getMaxValue()));
	Value<int32_t>::selectEncoderAction(offset);
}
void DurationSetting::getNotificationValue(StringBuf& value) {
	if (!getValue())
		value.append("Manual");
	else {
		value.appendInt(getValue());
		value.append("/");
		value.appendInt(getMaxValue());
		value.append(" bars");
	}
}

TransposeSetting::TransposeSetting(SettingsContext& context)
    : Decimal(String::STRING_FOR_LOOP_STATION_TRANSPOSE), context_(context) {
}
void TransposeSetting::writeCurrentValue() {
	context_.write(MenuField::Transpose, getValue());
	readCurrentValue();
}
int32_t TransposeSetting::getNumberEditSize() {
	return Buttons::isAnyOfButtonsPressed({hid::button::SHIFT, hid::button::SELECT_ENC}) ? 1 : 100;
}
void TransposeSetting::selectEncoderAction(int32_t offset) {
	readCurrentValue();
	if (parent && parent->renderingStyle() == Submenu::RenderingStyle::HORIZONTAL) {
		Decimal::selectEncoderAction(offset);
	}
	else {
		setValue(ls::stepTransposeCents(getValue(), offset, getNumberEditSize() == 1));
		Value<int32_t>::selectEncoderAction(offset);
	}
}

CopySetting::CopySetting(SettingsContext& context)
    : MenuItem(String::STRING_FOR_LOOP_STATION_COPY_TO_TRACK), context_(context) {
}

SettingsItems::SettingsItems(SettingsContext& context)
    : enums_{{{context, MenuField::RecordAction, String::STRING_FOR_LOOP_STATION_RECORD_ACTION},
              {context, MenuField::StartMode, String::STRING_FOR_LOOP_STATION_START_MODE},
              {context, MenuField::StartInterval, String::STRING_FOR_LOOP_STATION_START_INTERVAL},
              {context, MenuField::StopMode, String::STRING_FOR_LOOP_STATION_STOP_MODE},
              {context, MenuField::StopInterval, String::STRING_FOR_LOOP_STATION_STOP_INTERVAL},
              {context, MenuField::Source, String::STRING_FOR_LOOP_STATION_INPUT_SOURCE},
              {context, MenuField::Monitoring, String::STRING_FOR_LOOP_STATION_MONITORING},
              {context, MenuField::Playback, String::STRING_FOR_LOOP_STATION_PLAYBACK},
              {context, MenuField::Colour, String::STRING_FOR_LOOP_STATION_TRACK_COLOUR}}},
      duration_(context), transpose_(context), copy_(context),
      items_{&enums_[0], &enums_[1], &enums_[2], &enums_[3],  &enums_[4], &enums_[5],
             &enums_[6], &duration_, &enums_[7], &transpose_, &enums_[8], &copy_} {
}
MenuItem* SettingsItems::item(MenuField field) {
	const auto index = static_cast<size_t>(field);
	return index < items_.size() ? items_[index] : nullptr;
}
MenuField SettingsItems::field(const MenuItem* item) const {
	const auto found = std::find(items_.begin(), items_.end(), item);
	assert(found != items_.end());
	return static_cast<MenuField>(found - items_.begin());
}
} // namespace deluge::gui::menu_item::loop_station
