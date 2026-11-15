#include "loop_station_view.h"

#include "deluge/loop_station/geometry.h"
#include "deluge/loop_station/length_display.h"
#include "deluge/loop_station/song_model.h"
#include "deluge/loop_station/view_state.h"
#include "extern.h"
#include "gui/colour/palette.h"
#include "gui/l10n/l10n.h"
#include "gui/l10n/strings.h"
#include "gui/menu_item/loop_station/settings.h"
#include "gui/ui/ui.h"
#include "gui/views/session_view.h"
#include "gui/views/view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled_canvas/canvas.h"
#include "hid/led/indicator_leds.h"
#include "model/clip/audio_clip.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include <cstdio>

using namespace deluge::hid::button;
using namespace loop_station;

PLACE_SDRAM_BSS LoopStationView loopStationView{};

namespace {
constexpr int32_t kSidebarControlY = kDisplayHeight - 1;

SongModel* model() {
	return currentSong ? currentSong->loopStation() : nullptr;
}
char const* sourceLabel(loop_station::Source source) {
	switch (source) {
	case loop_station::Source::Left:
		return "LEFT";
	case loop_station::Source::Right:
		return "RIGHT";
	case loop_station::Source::Stereo:
		return "STEREO";
	case loop_station::Source::Balanced:
		return "BALANCED";
	}
	return "LEFT";
}

void formatTrackLength(const SlotState& state, uint32_t tempoQ16, char* out, size_t size) {
	if (state.capture)
		snprintf(out, size, "REC");
	else if (!state.hasPhrase)
		snprintf(out, size, "READY");
	else
		formatPhraseLength(state.phraseFrames, tempoQ16, out, size);
}
RGB colourFor(Colour c) {
	switch (c) {
	case Colour::Gold:
		return deluge::gui::colours::orange;
	case Colour::Yellow:
		return deluge::gui::colours::yellow;
	case Colour::Lime:
		return deluge::gui::colours::lime;
	case Colour::Turquoise:
		return deluge::gui::colours::turquoise;
	case Colour::Cyan:
		return deluge::gui::colours::cyan_full;
	case Colour::Sky:
		return deluge::gui::colours::darkblue;
	case Colour::Blue:
		return deluge::gui::colours::blue;
	case Colour::Purple:
		return deluge::gui::colours::purple;
	case Colour::Magenta:
		return deluge::gui::colours::magenta_full;
	case Colour::Pink:
		return RGB(255, 0, 128);
	case Colour::White:
		return deluge::gui::colours::white_full;
	}
	return deluge::gui::colours::white;
}
RGB scaledColour(RGB colour, uint8_t level) {
	return {static_cast<uint8_t>((static_cast<uint16_t>(colour.r) * level + 127) / 255),
	        static_cast<uint8_t>((static_cast<uint16_t>(colour.g) * level + 127) / 255),
	        static_cast<uint8_t>((static_cast<uint16_t>(colour.b) * level + 127) / 255)};
}

bool startPending(PendingAction action) {
	return action == PendingAction::Initial || action == PendingAction::Start;
}

bool stopPending(PendingAction action) {
	return action == PendingAction::FinishInitial || action == PendingAction::Stop
	       || action == PendingAction::ExitInitial || action == PendingAction::StopInitial
	       || action == PendingAction::Clear;
}

struct GroupAvailability {
	bool start = false;
	bool stop = false;
	bool pendingStart = false;
	bool pendingStop = false;
};

GroupAvailability groupAvailability(const Engine& engine) {
	GroupAvailability availability;
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		const auto state = engine.state(slot);
		availability.start |=
		    state.hasPhrase && !state.playing && !state.capture && !state.pending && state.error == Status::Ok;
		availability.stop |= state.playing || state.capture || state.pending;
		if (state.pending) {
			availability.pendingStart |= startPending(state.action);
			availability.pendingStop |= stopPending(state.action);
		}
	}
	return availability;
}

RGB destinationColour(PendingDestination destination) {
	switch (destination) {
	case PendingDestination::Capture:
		return deluge::gui::colours::red;
	case PendingDestination::Overdub:
		return deluge::gui::colours::orange;
	case PendingDestination::Play:
		return deluge::gui::colours::green;
	case PendingDestination::Stop:
		return deluge::gui::colours::green.dim(3);
	case PendingDestination::Delete:
		return deluge::gui::colours::magenta_full;
	case PendingDestination::None:
		return deluge::gui::colours::grey;
	}
	return deluge::gui::colours::grey;
}
} // namespace

bool LoopStationView::opened() {
	if (!currentSong || currentSong->ensureLoopStation(true) != Status::Ok)
		return false;
	updateLed();
	if (display && !display->haveOLED()) {
		display->setText("LOOP");
		display->displayPopup(kConfiguredMemoryProfile == MemoryProfile::TenSecond ? "10 SEC" : "12 SEC");
	}
	uiNeedsRendering(this);
	return true;
}
void LoopStationView::focusRegained() {
	updateLed();
}
void LoopStationView::updateLed() {
	indicator_leds::setLedState(IndicatorLED::CROSS_SCREEN_EDIT, true);
}
void LoopStationView::attemptExit() {
	auto* m = model();
	const auto status = m ? m->requestViewExit() : Status::Unavailable;
	if (status == Status::Ok) {
		pendingExit_ = false;
		changeRootUI(&sessionView);
	}
	else if (status == Status::Busy) {
		if (!pendingExit_)
			indicator_leds::blinkLed(IndicatorLED::SESSION_VIEW);
		pendingExit_ = true;
		if (display)
			display->displayPopup("EXIT PENDING");
	}
	else {
		pendingExit_ = false;
		indicator_leds::setLedState(IndicatorLED::SESSION_VIEW, false);
		if (display)
			display->displayPopup("EXIT FAILED");
	}
}
void LoopStationView::graphicsRoutine() {
	updateLed();
	uiNeedsRendering(this);
	if (display && display->haveOLED())
		renderUIsForOled();
}
ActionResult LoopStationView::timerCallback() {
	if (pendingExit_) {
		attemptExit();
		if (!pendingExit_ && getCurrentUI() != this)
			return ActionResult::DEALT_WITH;
	}
	if (auto* m = model(); m && pendingRestore_.valid()) {
		const auto status = m->serviceRestore(pendingRestore_, 128);
		if (status == Status::Ok)
			(void)m->commitRestore(pendingRestore_);
		else if (status != Status::Busy)
			m->cancelRestore(pendingRestoreSlot_);
		if (status != Status::Busy) {
			pendingRestore_ = {};
			pendingRestoreSlot_ = kInvalidSlot;
		}
	}
	graphicsRoutine();
	return ActionResult::DEALT_WITH;
}

bool LoopStationView::prepareTrack(SlotId slot) {
	auto* m = model();
	if (!m || !m->ready())
		return false;
	if (m->ensureBound(slot) == Status::Ok)
		return true;
	if (display)
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_LOOP_STATION_TRACK_UNAVAILABLE));
	return false;
}

ActionResult LoopStationView::padAction(int32_t x, int32_t y, int32_t velocity) {
	auto* m = model();
	if (!m || !m->ready())
		return ActionResult::DEALT_WITH;
	// Shared exclusive modes own the input. Consume new presses, but still pass
	// releases through so a gesture accepted before the mode changed can finish.
	if (velocity > 0 && currentUIMode != UI_MODE_NONE)
		return ActionResult::DEALT_WITH;
	if (x >= kDisplayWidth) {
		if (velocity > 0 && y == kSidebarControlY) {
			if (x == kDisplayWidth)
				(void)m->dispatch({CommandType::StartAll});
			else if (x == kDisplayWidth + 1)
				(void)m->dispatch({CommandType::StopAll});
			graphicsRoutine();
		}
		return ActionResult::DEALT_WITH;
	}
	const auto pad = padAddress(x, y);
	if (!pad.valid)
		return ActionResult::DEALT_WITH;
	if (velocity > 0 && sdRoutineLock)
		return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
	if (velocity > 0 && !prepareTrack(pad.slot))
		return ActionResult::DEALT_WITH;
	if (pad.region == PadRegion::Border) {
		if (velocity > 0) {
			(void)m->dispatch({CommandType::Select, pad.slot});
			graphicsRoutine();
		}
		return ActionResult::DEALT_WITH;
	}
	if (velocity > 0 && Buttons::isShiftButtonPressed()) {
		(void)deluge::gui::menu_item::loop_station::loopStationSettingsMenu.openTrack(pad.slot);
		return ActionResult::DEALT_WITH;
	}
	if (m->engine().state(pad.slot).error != Status::Ok) {
		if (velocity > 0)
			(void)m->dispatch({CommandType::Acknowledge, pad.slot});
	}
	else if (velocity > 0) {
		(void)m->dispatch({Buttons::isShiftButtonPressed() ? CommandType::ShiftPress : CommandType::Press, pad.slot});
	}
	else {
		(void)m->dispatch({CommandType::Release, pad.slot});
	}
	graphicsRoutine();
	return ActionResult::DEALT_WITH;
}

ActionResult LoopStationView::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	if (inCardRoutine)
		return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;

	const bool loopIdle = currentUIMode == UI_MODE_NONE;
	auto* m = model();

	// Retained loop shortcuts own both edges only while no shared mode is active.
	if (loopIdle) {
		if (b == CROSS_SCREEN_EDIT) {
			if (on && !Buttons::isShiftButtonPressed())
				attemptExit();
			return ActionResult::DEALT_WITH;
		}
		if (b == SESSION_VIEW) {
			if (on && !Buttons::isShiftButtonPressed())
				attemptExit();
			return ActionResult::DEALT_WITH;
		}
		if (b == CLIP_VIEW) {
			if (on && m) {
				const SlotId slot = m->engine().selected();
				if (validSlot(slot)) {
					if (auto* clip = m->clip(slot))
						sessionView.transitionToViewForClip(clip);
				}
			}
			return ActionResult::DEALT_WITH;
		}
		if (b == SELECT_ENC && !Buttons::isShiftButtonPressed()) {
			if (on)
				(void)deluge::gui::menu_item::loop_station::loopStationSettingsMenu.openGlobal();
			return ActionResult::DEALT_WITH;
		}
		if (b == BACK) {
			if (on && m) {
				const SlotId slot = m->engine().selected();
				RestoreId restore{};
				if (validSlot(slot) && m->canUndo(slot) && !pendingRestore_.valid()
				    && m->requestRestore(slot, false, restore) == Status::Ok) {
					pendingRestore_ = restore;
					pendingRestoreSlot_ = slot;
				}
			}
			return ActionResult::DEALT_WITH;
		}
	}

	// PLAY, RECORD, SHIFT and every other ordinary control are the shared/native
	// handler's responsibility. Propagate its result so the native dispatcher sees
	// NOT_DEALT_WITH unchanged.
	return view.buttonAction(b, on, inCardRoutine);
}

bool LoopStationView::renderMainPads(uint32_t rows, RGB image[][kDisplayWidth + kSideBarWidth],
                                     uint8_t mask[][kDisplayWidth + kSideBarWidth], bool) {
	if (!image || !mask)
		return true;
	auto* m = model();
	if (!m)
		return true;
	const auto now = AudioEngine::loopStationSampleClock();
	const auto level = selectionPulseLevel(now);
	const SlotId menuFocus = deluge::gui::menu_item::loop_station::loopStationSettingsMenu.focusedTrack();
	const SlotId selected = validSlot(menuFocus) ? menuFocus : m->engine().selected();
	for (int32_t y = 0; y < kDisplayHeight; ++y) {
		if (!(rows & (1u << y)))
			continue;
		for (int32_t x = 0; x < kDisplayWidth; ++x) {
			const auto pad = padAddress(x, y);
			if (!pad.valid)
				continue;
			const auto s = m->engine().state(pad.slot);
			const RGB c = colourFor(m->engine().settings().tracks[pad.slot].colour);
			const bool pulse = visualPulseOn(now);
			const bool fastPulse = visualPulseOn(now, 2205);
			const bool focused = pad.slot == selected;
			const auto held = m->engine().heldDurationSamples(pad.slot, now);
			const auto clearSweep = !s.playing && !s.capture && !s.pending && s.hasPhrase
			                            ? clearHoldSweepPads(held, 2ull * loop_station::kSampleRate)
			                            : 0;
			if (s.error != Status::Ok) {
				const RGB errorColour = deluge::gui::colours::red;
				image[y][x] = pad.region == PadRegion::Centre ? (fastPulse ? errorColour : deluge::gui::colours::black)
				                                              : errorColour.dim(3);
			}
			else if (pad.region == PadRegion::Centre && s.pending) {
				const RGB destination =
				    destinationColour(pendingDestination(s.action, m->engine().settings().recordAction));
				image[y][x] = pulse ? destination : destination.dim(4);
			}
			else if (pad.region == PadRegion::Centre) {
				image[y][x] = s.capture     ? (s.playing ? deluge::gui::colours::orange : deluge::gui::colours::red)
				              : clearSweep  ? deluge::gui::colours::red
				              : s.playing   ? deluge::gui::colours::green
				              : s.hasPhrase ? deluge::gui::colours::green.dim(3)
				                            : RGB::monochrome(40);
			}
			else if (s.capture) {
				const RGB captureColour = s.playing ? deluge::gui::colours::orange : deluge::gui::colours::red;
				image[y][x] = pulse ? captureColour : captureColour.dim(4);
			}
			else if (s.pending) {
				const RGB destination =
				    destinationColour(pendingDestination(s.action, m->engine().settings().recordAction));
				image[y][x] = pulse ? destination : destination.dim(4);
			}
			else if (clearSweep) {
				const RGB clearColour = deluge::gui::colours::red;
				image[y][x] = pad.borderIndex < clearSweep ? clearColour : clearColour.dim(3);
			}
			else if (s.playing && s.phraseFrames) {
				const bool filled = pad.borderIndex < filledBorderPads(m->adapter().phase(pad.slot), s.phraseFrames);
				image[y][x] = focused  ? scaledColour(c, filled ? 128 + level / 2 : 32 + level / 8)
				              : filled ? c
				                       : c.dim(3);
			}
			else
				image[y][x] = focused ? scaledColour(c, 32 + level / 3) : c.dim(3);
			mask[y][x] = 1;
		}
	}
	return true;
}

bool LoopStationView::renderSidebar(uint32_t rows, RGB image[][kDisplayWidth + kSideBarWidth],
                                    uint8_t mask[][kDisplayWidth + kSideBarWidth]) {
	if (!image || !mask)
		return true;
	auto* m = model();
	const auto groups = m ? groupAvailability(m->engine()) : GroupAvailability{};
	const bool pulse = visualPulseOn(AudioEngine::loopStationSampleClock());
	for (int32_t y = 0; y < kDisplayHeight; ++y) {
		if (!(rows & (1u << y)))
			continue;
		for (int32_t x = kDisplayWidth; x < kDisplayWidth + kSideBarWidth; ++x) {
			image[y][x] = deluge::gui::colours::black;
			if (y == kSidebarControlY && x == kDisplayWidth) {
				const RGB active = deluge::gui::colours::green;
				image[y][x] = groups.pendingStart && pulse ? deluge::gui::colours::white_full
				              : groups.start               ? active
				                                           : active.dim(3);
			}
			else if (y == kSidebarControlY && x == kDisplayWidth + 1) {
				const RGB active = deluge::gui::colours::red;
				image[y][x] = groups.pendingStop && pulse ? deluge::gui::colours::white_full
				              : groups.stop               ? active
				                                          : active.dim(3);
			}
			mask[y][x] = 1;
		}
	}
	return true;
}

void LoopStationView::renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	canvas.clear();
	canvas.drawStringCentred("LOOP STATION", OLED_MAIN_TOPMOST_PIXEL + 1, kTextTitleSpacingX, kTextTitleSizeY);
	auto* m = model();
	if (!m)
		return;

	const SlotId selected = m->engine().selected();
	const bool hasTrack = validSlot(selected);
	char line[24];
	if (hasTrack)
		snprintf(line, sizeof(line), "TRACK %u", static_cast<unsigned>(selected) + 1);
	else
		snprintf(line, sizeof(line), "NO TRACK");
	canvas.drawString(line, 0, 20, kTextSpacingX, kTextSizeYUpdated);

	const uint32_t bpm = (m->clock().tempoQ16() + 32768) / 65536;
	snprintf(line, sizeof(line), "%u BPM", bpm);
	canvas.drawStringAlignRight(line, 20, kTextSpacingX, kTextSizeYUpdated);

	if (!hasTrack)
		return;

	snprintf(line, sizeof(line), "IN: %s", sourceLabel(m->engine().settings().tracks[selected].source));
	canvas.drawString(line, 0, 30, kTextSpacingX, kTextSizeYUpdated);

	formatTrackLength(m->engine().state(selected), m->clock().tempoQ16(), line, sizeof(line));
	canvas.drawStringAlignRight(line, 30, kTextSpacingX, kTextSizeYUpdated);
}
void LoopStationView::selectEncoderAction(int8_t offset) {
	if (currentUIMode != UI_MODE_NONE)
		return;
	if (auto* m = model()) {
		int selected = m->engine().selected();
		if (!validSlot(selected))
			selected = 0;
		selected = (selected + offset + kTrackCount) % kTrackCount;
		const auto slot = static_cast<SlotId>(selected);
		if (prepareTrack(slot)) {
			(void)m->dispatch({CommandType::Select, slot});
			graphicsRoutine();
		}
	}
}
