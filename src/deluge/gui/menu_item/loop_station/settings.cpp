#include "settings.h"

#include "deluge/loop_station/song_model.h"
#include "extern.h"
#include "gui/l10n/l10n.h"
#include "gui/l10n/strings.h"
#include "gui/ui/ui.h"
#include "hid/button.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled_canvas/canvas.h"
#include "model/song/song.h"
#include <algorithm>
#include <array>
#include <cstdio>

namespace deluge::gui::menu_item::loop_station {
using namespace deluge::hid::button;
using namespace ::loop_station;

namespace {
SongModel* currentModel() {
	return currentSong ? currentSong->loopStation() : nullptr;
}

const char* fieldName(MenuField field) {
	using deluge::l10n::String;
	switch (field) {
	case MenuField::RecordAction:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_RECORD_ACTION);
	case MenuField::StartMode:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_START_MODE);
	case MenuField::StartInterval:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_START_INTERVAL);
	case MenuField::StopMode:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_STOP_MODE);
	case MenuField::StopInterval:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_STOP_INTERVAL);
	case MenuField::Source:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_INPUT_SOURCE);
	case MenuField::Monitoring:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_MONITORING);
	case MenuField::Duration:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_DURATION);
	case MenuField::Playback:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_PLAYBACK);
	case MenuField::Transpose:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_TRANSPOSE);
	case MenuField::Colour:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_TRACK_COLOUR);
	case MenuField::CopyToTrack:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_COPY_TO_TRACK);
	}
	return "Setting";
}

const char* intervalName(Interval interval) {
	switch (interval) {
	case Interval::Bar:
		return "Bar";
	case Interval::Half:
		return "1/2";
	case Interval::Quarter:
		return "1/4";
	case Interval::Eighth:
		return "1/8";
	case Interval::BarTriplet:
		return "Bar triplet";
	case Interval::HalfTriplet:
		return "1/2 triplet";
	case Interval::QuarterTriplet:
		return "1/4 triplet";
	case Interval::EighthTriplet:
		return "1/8 triplet";
	case Interval::Sixteenth:
		return "1/16";
	case Interval::ThirtySecond:
		return "1/32";
	case Interval::SixteenthTriplet:
		return "1/16 triplet";
	case Interval::ThirtySecondTriplet:
		return "1/32 triplet";
	}
	return "1/16";
}

const char* colourName(Colour colour) {
	static constexpr std::array<const char*, kTrackColourCount> names = {
	    "Gold", "Yellow", "Lime", "Turquoise", "Cyan", "Sky", "Blue", "Purple", "Magenta", "Pink", "White"};
	for (size_t i = 0; i < kTrackColours.size(); ++i)
		if (kTrackColours[i] == colour)
			return names[i];
	return "Yellow";
}

int wrapIndex(int value, int count) {
	return count ? ((value % count) + count) % count : 0;
}
} // namespace

bool SettingsMenu::openGlobal() {
	return open(MenuScope::Global, 0);
}
bool SettingsMenu::openTrack(SlotId slot) {
	return validSlot(slot) && open(MenuScope::Track, slot);
}

bool SettingsMenu::open(MenuScope scope, SlotId slot) {
	scope_ = scope;
	slot_ = slot;
	copyTarget_ = static_cast<SlotId>((slot + 1) % kTrackCount);
	row_ = 0;
	editing_ = false;
	choosingCopyTarget_ = false;
	return openUI(this);
}

bool SettingsMenu::opened() {
	if (!currentSong || currentSong->ensureLoopStation(true) != Status::Ok)
		return false;
	auto* model = currentModel();
	if (!model)
		return false;
	if (scope_ == MenuScope::Track && model->ensureBound(slot_) != Status::Ok) {
		if (display)
			display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_LOOP_STATION_TRACK_UNAVAILABLE));
		return false;
	}
	fields_ = visibleMenuFields(scope_, model->engine().settings());
	refreshDisplay();
	return true;
}

ActionResult SettingsMenu::buttonAction(deluge::hid::Button button, bool on, bool inCardRoutine) {
	if (inCardRoutine)
		return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
	if (!on)
		return ActionResult::DEALT_WITH;
	if (shouldCloseSettingsMenu(button == BACK, button == SESSION_VIEW, Buttons::isShiftButtonPressed())) {
		closeMenu();
		return ActionResult::DEALT_WITH;
	}
	if (button == SELECT_ENC) {
		selectCurrent();
		return ActionResult::DEALT_WITH;
	}
	return ActionResult::DEALT_WITH;
}

ActionResult SettingsMenu::padAction(int32_t, int32_t, int32_t velocity) {
	if (velocity > 0)
		selectCurrent();
	return ActionResult::DEALT_WITH;
}

ActionResult SettingsMenu::horizontalEncoderAction(int32_t offset) {
	selectEncoderAction(static_cast<int8_t>(std::clamp<int32_t>(offset, -127, 127)));
	return ActionResult::DEALT_WITH;
}

void SettingsMenu::selectEncoderAction(int8_t offset) {
	if (choosingCopyTarget_) {
		copyTarget_ = static_cast<SlotId>(wrapIndex(copyTarget_ + offset, kTrackCount));
	}
	else if (editing_) {
		changeValue(offset);
	}
	else if (fields_.count) {
		row_ = static_cast<uint8_t>(wrapIndex(row_ + offset, fields_.count));
	}
	refreshDisplay();
	uiNeedsRendering(this);
}

void SettingsMenu::selectCurrent() {
	if (!fields_.count)
		return;
	const auto field = fields_.rows[row_];
	if (field == MenuField::CopyToTrack && scope_ == MenuScope::Track) {
		if (!choosingCopyTarget_) {
			choosingCopyTarget_ = true;
			refreshDisplay();
			return;
		}
		auto* model = currentModel();
		if (!model || copyTarget_ == slot_)
			return;
		const auto status = model->copySettingsTo(slot_, copyTarget_);
		if (display)
			display->displayPopup(deluge::l10n::get(status == Status::Ok
			                                            ? deluge::l10n::String::STRING_FOR_LOOP_STATION_COPIED
			                                            : deluge::l10n::String::STRING_FOR_LOOP_STATION_COPY_BUSY));
		choosingCopyTarget_ = false;
		refreshDisplay();
		return;
	}
	if (choosingCopyTarget_) {
		choosingCopyTarget_ = false;
		refreshDisplay();
		return;
	}
	editing_ = !editing_;
	refreshDisplay();
}

void SettingsMenu::changeValue(int32_t offset) {
	auto* model = currentModel();
	if (!model || (scope_ == MenuScope::Track && !validSlot(slot_)) || !fields_.count)
		return;
	Settings settings = model->engine().settings();
	const auto field = fields_.rows[row_];
	auto& track = settings.tracks[slot_];
	auto changeInterval = [offset](ActionSettings& action) {
		const auto oldIndex = static_cast<int32_t>(menuIndexOfInterval(action.interval));
		action.interval =
		    intervalAtMenuIndex(static_cast<unsigned>(wrapIndex(oldIndex + offset, kIntervalMenuOrder.size())));
	};
	switch (field) {
	case MenuField::RecordAction:
		settings.recordAction = settings.recordAction == RecordAction::REC_PLAY_DUB ? RecordAction::REC_DUB_PLAY
		                                                                            : RecordAction::REC_PLAY_DUB;
		break;
	case MenuField::StartMode:
		settings.startAction.mode =
		    settings.startAction.mode == ActionMode::Synced ? ActionMode::Immediate : ActionMode::Synced;
		break;
	case MenuField::StartInterval:
		changeInterval(settings.startAction);
		break;
	case MenuField::StopMode:
		settings.stopAction.mode =
		    settings.stopAction.mode == ActionMode::Synced ? ActionMode::Immediate : ActionMode::Synced;
		break;
	case MenuField::StopInterval:
		changeInterval(settings.stopAction);
		break;
	case MenuField::Source:
		track.source = static_cast<::loop_station::Source>(wrapIndex(static_cast<int>(track.source) + offset, 4));
		break;
	case MenuField::Monitoring:
		track.monitoring = static_cast<Monitoring>(wrapIndex(static_cast<int>(track.monitoring) + offset, 2));
		break;
	case MenuField::Duration: {
		const auto barFrames = model->clock().samplesForBars(1);
		const auto maxBars = model->maxDurationBarsForTrack(slot_, barFrames);
		track.recordDurationBars =
		    static_cast<uint16_t>(std::clamp<int32_t>(track.recordDurationBars + offset, 0, maxBars));
		break;
	}
	case MenuField::Playback:
		track.playback =
		    static_cast<::loop_station::PlaybackMode>(wrapIndex(static_cast<int>(track.playback) + offset, 2));
		break;
	case MenuField::Transpose:
		track.transposeCents = stepTransposeCents(track.transposeCents, offset);
		break;
	case MenuField::Colour:
		track.colour = trackColourAt(static_cast<unsigned>(
		    wrapIndex(static_cast<int>(std::find(kTrackColours.begin(), kTrackColours.end(), track.colour)
		                               - kTrackColours.begin())
		                  + offset,
		              kTrackColourCount)));
		break;
	case MenuField::CopyToTrack:
		return;
	}
	(void)model->dispatch(Command::configure(settings));
	fields_ = visibleMenuFields(scope_, model->engine().settings());
	if (row_ >= fields_.count)
		row_ = fields_.count ? fields_.count - 1 : 0;
	refreshDisplay();
}

void SettingsMenu::refreshDisplay() {
	if (!display || display->haveOLED())
		return;
	display->setText(deluge::l10n::get(scope_ == MenuScope::Global
	                                       ? deluge::l10n::String::STRING_FOR_LOOP_STATION_GLOBAL_SETTINGS
	                                       : deluge::l10n::String::STRING_FOR_LOOP_STATION_TRACK_SETTINGS));
	if (choosingCopyTarget_) {
		char target[16];
		snprintf(target, sizeof(target), "TO TR %u", copyTarget_ + 1);
		display->displayPopup(target);
		return;
	}
	if (!fields_.count)
		return;
	auto* model = currentModel();
	if (!model)
		return;
	const auto field = fields_.rows[row_];
	char value[24]{};
	if (scope_ == MenuScope::Global) {
		const auto& settings = model->engine().settings();
		switch (field) {
		case MenuField::RecordAction:
			snprintf(value, sizeof(value), "%s",
			         settings.recordAction == RecordAction::REC_PLAY_DUB ? "PLAY DUB" : "DUB PLAY");
			break;
		case MenuField::StartMode:
			snprintf(value, sizeof(value), "%s", settings.startAction.mode == ActionMode::Synced ? "SYNC" : "IMM");
			break;
		case MenuField::StartInterval:
			snprintf(value, sizeof(value), "%s", intervalName(settings.startAction.interval));
			break;
		case MenuField::StopMode:
			snprintf(value, sizeof(value), "%s", settings.stopAction.mode == ActionMode::Synced ? "SYNC" : "IMM");
			break;
		case MenuField::StopInterval:
			snprintf(value, sizeof(value), "%s", intervalName(settings.stopAction.interval));
			break;
		default:
			break;
		}
	}
	else {
		const auto& track = model->engine().settings().tracks[slot_];
		switch (field) {
		case MenuField::Source:
			snprintf(value, sizeof(value), "SRC %u", static_cast<unsigned>(track.source) + 1);
			break;
		case MenuField::Monitoring:
			snprintf(value, sizeof(value), "%s", track.monitoring == Monitoring::On ? "MON ON" : "MON OFF");
			break;
		case MenuField::Duration: {
			const auto maxBars = model->maxDurationBarsForTrack(slot_, model->clock().samplesForBars(1));
			if (track.recordDurationBars)
				snprintf(value, sizeof(value), "%u/%u BAR", track.recordDurationBars, maxBars);
			else
				snprintf(value, sizeof(value), "MANUAL");
			break;
		}
		case MenuField::Playback:
			snprintf(value, sizeof(value), "%s",
			         track.playback == ::loop_station::PlaybackMode::Loop ? "LOOP" : "1 SHOT");
			break;
		case MenuField::Transpose:
			snprintf(value, sizeof(value), "%d C", track.transposeCents);
			break;
		case MenuField::Colour:
			snprintf(value, sizeof(value), "%s", colourName(track.colour));
			break;
		case MenuField::CopyToTrack:
			snprintf(value, sizeof(value), "TO TR %u", copyTarget_ + 1);
			break;
		default:
			break;
		}
	}
	display->displayPopup(value);
}

bool SettingsMenu::renderMainPads(uint32_t, RGB[][kDisplayWidth + kSideBarWidth],
                                  uint8_t[][kDisplayWidth + kSideBarWidth], bool) {
	return false;
}
bool SettingsMenu::renderSidebar(uint32_t, RGB[][kDisplayWidth + kSideBarWidth],
                                 uint8_t[][kDisplayWidth + kSideBarWidth]) {
	return false;
}

void SettingsMenu::renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	canvas.clear();
	canvas.drawString(deluge::l10n::get(scope_ == MenuScope::Global
	                                        ? deluge::l10n::String::STRING_FOR_LOOP_STATION_GLOBAL_SETTINGS
	                                        : deluge::l10n::String::STRING_FOR_LOOP_STATION_TRACK_SETTINGS),
	                  0, OLED_MAIN_TOPMOST_PIXEL + 1, kTextTitleSpacingX, kTextTitleSizeY);
	auto* model = currentModel();
	if (!model)
		return;
	if (choosingCopyTarget_) {
		char target[24];
		snprintf(target, sizeof(target), "Copy to Track %u", copyTarget_ + 1);
		canvas.drawString(target, 0, 22, kTextSpacingX, kTextSizeYUpdated);
		canvas.drawString("SELECT to copy", 0, 38, kTextSpacingX, kTextSizeYUpdated);
		return;
	}
	fields_ = visibleMenuFields(scope_, model->engine().settings());
	const uint8_t first = std::min<uint8_t>(row_ > 0 ? row_ - 1 : 0, fields_.count > 3 ? fields_.count - 3 : 0);
	const uint8_t end = std::min<uint8_t>(first + 3, fields_.count);
	for (uint8_t i = first; i < end; ++i) {
		char value[32]{};
		const auto field = fields_.rows[i];
		const auto& settings = model->engine().settings();
		if (scope_ == MenuScope::Global) {
			switch (field) {
			case MenuField::RecordAction:
				snprintf(value, sizeof(value), "%s",
				         settings.recordAction == RecordAction::REC_PLAY_DUB ? "REC PLAY DUB" : "REC DUB PLAY");
				break;
			case MenuField::StartMode:
				snprintf(value, sizeof(value), "%s",
				         settings.startAction.mode == ActionMode::Synced ? "Synced" : "Immediate");
				break;
			case MenuField::StartInterval:
				snprintf(value, sizeof(value), "%s", intervalName(settings.startAction.interval));
				break;
			case MenuField::StopMode:
				snprintf(value, sizeof(value), "%s",
				         settings.stopAction.mode == ActionMode::Synced ? "Synced" : "Immediate");
				break;
			case MenuField::StopInterval:
				snprintf(value, sizeof(value), "%s", intervalName(settings.stopAction.interval));
				break;
			default:
				break;
			}
		}
		else {
			const auto& track = settings.tracks[slot_];
			switch (field) {
			case MenuField::Source: {
				static constexpr const char* sources[] = {"Left", "Right", "Stereo", "Balanced"};
				snprintf(value, sizeof(value), "%s", sources[static_cast<unsigned>(track.source)]);
				break;
			}
			case MenuField::Monitoring:
				snprintf(value, sizeof(value), "%s", track.monitoring == Monitoring::On ? "On" : "Off");
				break;
			case MenuField::Duration: {
				const auto maxBars = model->maxDurationBarsForTrack(slot_, model->clock().samplesForBars(1));
				if (track.recordDurationBars)
					snprintf(value, sizeof(value), "%u/%u BARS", track.recordDurationBars, maxBars);
				else
					snprintf(value, sizeof(value), "MANUAL");
				break;
			}
			case MenuField::Playback:
				snprintf(value, sizeof(value), "%s",
				         track.playback == ::loop_station::PlaybackMode::Loop ? "Loop" : "One Shot");
				break;
			case MenuField::Transpose:
				snprintf(value, sizeof(value), "%+.2f st", track.transposeCents / 100.0);
				break;
			case MenuField::Colour:
				snprintf(value, sizeof(value), "%s", colourName(track.colour));
				break;
			case MenuField::CopyToTrack:
				snprintf(value, sizeof(value), "Track %u", copyTarget_ + 1);
				break;
			default:
				break;
			}
		}
		const int32_t y = 20 + (i - first) * kTextSpacingY;
		char line[64];
		snprintf(line, sizeof(line), "%c%s: %s", i == row_ ? (editing_ ? '*' : '>') : ' ', fieldName(field), value);
		canvas.drawString(line, 0, y, kTextSpacingX, kTextSizeYUpdated);
	}
}

void SettingsMenu::closeMenu() {
	close();
}

} // namespace deluge::gui::menu_item::loop_station
