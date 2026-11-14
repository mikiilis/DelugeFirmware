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
#include "model/settings/runtime_feature_settings.h"
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
	if (horizontalSession_) {
		horizontalMenu_.endSession();
		horizontalSession_ = false;
	}
	scope_ = scope;
	slot_ = slot;
	context_.scope = scope;
	context_.slot = slot;
	copyTarget_ = static_cast<SlotId>((slot + 1) % kTrackCount);
	row_ = 0;
	editing_ = false;
	choosingCopyTarget_ = false;
	copyReview_ = false;
	selectHeld_ = false;
	const bool result = openUI(this);
	if (!result) {
		if (horizontalSession_)
			horizontalMenu_.endSession();
		horizontalSession_ = false;
	}
	return result;
}

bool SettingsMenu::useHorizontalMenu() const {
	return display && display->haveOLED() && runtimeFeatureSettings.isOn(HorizontalMenus);
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
	if (useHorizontalMenu()) {
		horizontalMenu_.resetSelection();
		horizontalMenu_.beginSession(nullptr);
		horizontalSession_ = true;
	}
	refreshDisplay();
	return true;
}

ActionResult SettingsMenu::buttonAction(deluge::hid::Button button, bool on, bool inCardRoutine) {
	// Physical releases only clear the press latch; they never dispatch a command.
	if (button == SELECT_ENC && !on) {
		selectHeld_ = false;
		return ActionResult::DEALT_WITH;
	}
	if (inCardRoutine)
		return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
	if (button == SHIFT)
		return ActionResult::NOT_DEALT_WITH;
	if (button == SELECT_ENC) {
		if (sdRoutineLock)
			return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
		if (selectHeld_)
			return ActionResult::DEALT_WITH;
		selectHeld_ = true;
		if (copyReview_)
			confirmCopy();
		else if (useHorizontalMenu()) {
			if (horizontalMenu_.selectedField() == MenuField::CopyToTrack)
				requestCopyReview();
		}
		else
			selectCurrent();
		return ActionResult::DEALT_WITH;
	}
	if (!on)
		return ActionResult::DEALT_WITH;
	if (button == BACK && copyReview_) {
		cancelCopyReview();
		return ActionResult::DEALT_WITH;
	}
	if (shouldCloseSettingsMenu(button == BACK, button == SESSION_VIEW, Buttons::isShiftButtonPressed())) {
		closeMenu();
		return ActionResult::DEALT_WITH;
	}
	if (useHorizontalMenu()) {
		if (!copyReview_) {
			horizontalMenu_.renderOLED(); // Refresh relevance and shared page mapping before input.
			horizontalMenu_.buttonAction(button, on, inCardRoutine);
			refreshDisplay();
		}
		return ActionResult::DEALT_WITH;
	}
	return ActionResult::DEALT_WITH;
}

ActionResult SettingsMenu::padAction(int32_t, int32_t, int32_t) {
	return ActionResult::DEALT_WITH;
}

ActionResult SettingsMenu::horizontalEncoderAction(int32_t offset) {
	if (!useHorizontalMenu())
		selectEncoderAction(static_cast<int8_t>(std::clamp<int32_t>(offset, -127, 127)));
	return ActionResult::DEALT_WITH;
}

void SettingsMenu::selectEncoderAction(int8_t offset) {
	if (sdRoutineLock || copyReview_)
		return;
	if (useHorizontalMenu()) {
		if (horizontalMenu_.selectedField() == MenuField::CopyToTrack) {
			// The source is never a destination: rotate a seven-entry ring.
			const int index = copyTarget_ < slot_ ? copyTarget_ : copyTarget_ - 1;
			const int next = wrapIndex(index + offset, kTrackCount - 1);
			copyTarget_ = static_cast<SlotId>(next < slot_ ? next : next + 1);
		}
		else {
			const auto field = horizontalMenu_.selectedField();
			horizontalMenu_.selectEncoderAction(offset);
			horizontalMenu_.selectField(field); // Identity survives inserted/removed intervals.
		}
		refreshDisplay();
		uiNeedsRendering(getRootUI());
		return;
	}
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
		requestCopyReview();
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

void SettingsMenu::requestCopyReview() {
	if (scope_ != MenuScope::Track || copyTarget_ == slot_)
		return;
	copyReviewTarget_ = copyTarget_;
	copyReview_ = true;
	refreshDisplay();
}

void SettingsMenu::confirmCopy() {
	if (!copyReview_ || sdRoutineLock)
		return;
	auto* model = currentModel();
	const auto status = model ? model->copySettingsTo(slot_, copyReviewTarget_) : Status::Unavailable;
	copyReview_ = choosingCopyTarget_ = false;
	refreshDisplay();
	if (display)
		display->displayPopup(deluge::l10n::get(status == Status::Ok
		                                            ? deluge::l10n::String::STRING_FOR_LOOP_STATION_COPIED
		                                            : deluge::l10n::String::STRING_FOR_LOOP_STATION_COPY_BUSY));
}

void SettingsMenu::cancelCopyReview() {
	copyReview_ = choosingCopyTarget_ = false;
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
		track.transposeCents =
		    stepTransposeCents(track.transposeCents, offset, Buttons::isAnyOfButtonsPressed({SHIFT, SELECT_ENC}));
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
	const auto status = model->dispatch(Command::configure(settings));
	if (status == Status::Ok && field == MenuField::Colour)
		uiNeedsRendering(getRootUI());
	fields_ = visibleMenuFields(scope_, model->engine().settings());
	if (row_ >= fields_.count)
		row_ = fields_.count ? fields_.count - 1 : 0;
	refreshDisplay();
}

void SettingsMenu::refreshDisplay() {
	if (!display)
		return;
	if (display->haveOLED()) {
		renderUIsForOled();
		return;
	}
	display->setText(deluge::l10n::get(scope_ == MenuScope::Global
	                                       ? deluge::l10n::String::STRING_FOR_LOOP_STATION_GLOBAL_SETTINGS
	                                       : deluge::l10n::String::STRING_FOR_LOOP_STATION_TRACK_SETTINGS));
	if (copyReview_ || choosingCopyTarget_) {
		char target[16];
		snprintf(target, sizeof(target), copyReview_ ? "COPY TR %u?" : "TO TR %u",
		         (copyReview_ ? copyReviewTarget_ : copyTarget_) + 1);
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
	if (useHorizontalMenu() && !copyReview_) {
		static_cast<CopySetting*>(items_.item(MenuField::CopyToTrack))->setTarget(copyTarget_);
		horizontalMenu_.renderOLED();
		return;
	}
	renderListOLED(canvas);
}

void SettingsMenu::renderListOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	canvas.drawString(deluge::l10n::get(scope_ == MenuScope::Global
	                                        ? deluge::l10n::String::STRING_FOR_LOOP_STATION_GLOBAL_SETTINGS
	                                        : deluge::l10n::String::STRING_FOR_LOOP_STATION_TRACK_SETTINGS),
	                  0, OLED_MAIN_TOPMOST_PIXEL + 1, kTextTitleSpacingX, kTextTitleSizeY);
	auto* model = currentModel();
	if (!model)
		return;
	if (copyReview_ || choosingCopyTarget_) {
		char target[24];
		snprintf(target, sizeof(target), "Copy to Track %u", (copyReview_ ? copyReviewTarget_ : copyTarget_) + 1);
		canvas.drawString(target, 0, 22, kTextSpacingX, kTextSizeYUpdated);
		canvas.drawString(copyReview_ ? "SELECT to copy" : "SELECT to review", 0, 38, kTextSpacingX, kTextSizeYUpdated);
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
	if (horizontalSession_)
		horizontalMenu_.endSession();
	horizontalSession_ = false;
	copyReview_ = choosingCopyTarget_ = selectHeld_ = false;
	close(); // closeUI restores parent LEDs after shared-menu LED cleanup.
}

} // namespace deluge::gui::menu_item::loop_station
