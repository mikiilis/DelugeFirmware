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
#include "hid/display/numeric_layer/numeric_layer_scrolling_text.h"
#include "hid/display/oled_canvas/canvas.h"
#include "hid/led/indicator_leds.h"
#include "model/clip/audio_clip.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include <cstdio>
#include <cstring>

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

// Terminal feedback for the observed model result. Only a committed Ok publishes
// a completion message; every other status is reported honestly.
char const* historyTerminalText(Status status, bool redo) {
	using deluge::l10n::String;
	switch (status) {
	case Status::Ok:
		return deluge::l10n::get(redo ? String::STRING_FOR_LOOP_STATION_REDONE
		                              : String::STRING_FOR_LOOP_STATION_UNDONE);
	case Status::Busy:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_HISTORY_BUSY);
	case Status::NoMemory:
		return deluge::l10n::get(String::STRING_FOR_LOOP_STATION_NO_MEMORY);
	default:
		return deluge::l10n::get(redo ? String::STRING_FOR_LOOP_STATION_REDO_UNAVAILABLE
		                              : String::STRING_FOR_LOOP_STATION_UNDO_UNAVAILABLE);
	}
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

// Shared exclusive modes own input. Documented non-exclusive flags (quantize, stutter,
// horizontal scroll, auditioning, held horizontal encoder) may coexist with loop
// performance and navigation, so they must not suppress these retained controls.
bool sharedExclusiveModeActive() {
	return (currentUIMode & EXCLUSIVE_UI_MODES_MASK) != 0;
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
	updateModTarget();
	if (display && !display->haveOLED()) {
		display->setText("LOOP");
		display->displayPopup(kConfiguredMemoryProfile == MemoryProfile::TenSecond ? "10 SEC" : "12 SEC");
	}
	uiNeedsRendering(this);
	return true;
}
void LoopStationView::focusRegained() {
	// A shared modal is made current before its opened() runs, so an accepted centre
	// release can be consumed during that I/O. If opening then fails, openUI restores
	// this view and calls here. Clear the stale hold through the current active model
	// before the ordinary refresh, so a later release cannot start or clear the
	// interrupted gesture. The physically still-held gesture must be repeated.
	if (auto* m = model(); m)
		m->cancelHeldPadGestures();
	// A prompt belongs to the view that armed it: any focus loss/reentry drops it so
	// returning from a menu can never revive an old confirmation.
	cancelHistoryConfirmation();
	historyBackDown_ = false;
	updateLed();
	updateModTarget();
}
bool LoopStationView::getAffectEntire() {
	return currentSong && currentSong->affectEntire;
}
UIModControllableContext LoopStationView::getUIModControllableContext() {
	if (!currentSong)
		return UIModControllableContext::NONE;
	return modTargetIsClip_ ? UIModControllableContext::CLIP : UIModControllableContext::SONG;
}
void LoopStationView::updateModTarget() {
	if (!currentSong) {
		const bool targetChanges = view.activeModControllableModelStack.timelineCounterIsSet();
		if (targetChanges)
			finishStutterBeforeTargetChange();
		modTargetIsClip_ = false;
		view.setActiveModControllableTimelineCounter(nullptr);
		return;
	}
	// Honor the existing song-wide mode. Only a persistent selected binding may
	// retarget effects; refreshing must never allocate a clip of its own.
	AudioClip* clip = nullptr;
	if (!currentSong->affectEntire) {
		if (auto* m = model(); m) {
			const SlotId slot = m->engine().selected();
			if (validSlot(slot))
				clip = m->clip(slot);
		}
	}
	TimelineCounter* currentTarget = view.activeModControllableModelStack.timelineCounterIsSet()
	                                     ? view.activeModControllableModelStack.getTimelineCounter()
	                                     : nullptr;
	TimelineCounter* nextTarget =
	    clip ? static_cast<TimelineCounter*>(clip) : static_cast<TimelineCounter*>(currentSong);
	if (currentTarget != nextTarget)
		finishStutterBeforeTargetChange();
	if (clip) {
		currentSong->setCurrentClip(clip);
		modTargetIsClip_ = true;
		view.setActiveModControllableTimelineCounter(clip);
	}
	else {
		modTargetIsClip_ = false;
		// Loop Station's explicit no-selection fallback always targets Song FX;
		// this does not mutate affectEntire or alter generic Song policy.
		view.setActiveModControllableTimelineCounter(currentSong, true, true);
	}
}
void LoopStationView::finishStutterBeforeTargetChange() {
	view.finishActiveStutterForModTargetChange();
}
void LoopStationView::updateLed() {
	// BACK reflects the selected track's eligible operation for the current
	// modifier: Undo normally, Redo with SHIFT. Never imply unavailable history.
	const bool shift = Buttons::isShiftButtonPressed();
	bool backAvailable = false;
	if (auto* m = model(); m && !sharedExclusiveModeActive()) {
		const SlotId slot = m->engine().selected();
		if (validSlot(slot))
			backAvailable = shift ? m->canRedo(slot) : m->canUndo(slot);
	}
	indicator_leds::setLedState(IndicatorLED::BACK, backAvailable);
	indicator_leds::setLedState(IndicatorLED::CROSS_SCREEN_EDIT, true);
}
void LoopStationView::attemptExit() {
	auto* m = model();
	const auto status = m ? m->requestViewExit() : Status::Unavailable;
	if (status == Status::Ok) {
		pendingExit_ = false;
		// Restore the outgoing modulation owner before the destination view
		// installs its target stack.
		finishStutterBeforeTargetChange();
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
	observeHistoryResult();
	if (sharedExclusiveModeActive())
		cancelHistoryConfirmation();
	updateLed();
	refreshHistoryFeedback();
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
	graphicsRoutine();
	return ActionResult::DEALT_WITH;
}

void LoopStationView::cancelHistoryConfirmation() {
	historyPromptState_ = HistoryPromptState::None;
	historyTarget_ = {};
	historyId_ = {};
	historyStatus_ = Status::Ok;
	clearHistoryFeedback();
}

void LoopStationView::clearHistoryFeedback() {
	if (!historyFeedbackOwned_) {
		historyFeedbackLayer_ = nullptr;
		historyFeedbackText_[0] = '\0';
		return;
	}
	// Removing the exact owned overlay is allocation-free: a failing allocator can
	// never strand canceled text, and newer layers, menus and popups are untouched.
	// When another UI owns the display it has already replaced our layer stack, so
	// forgetting ownership there cannot leave this view's text showing.
	if (display && !display->haveOLED() && getCurrentUI() == this) {
		display->removeLayer(historyFeedbackLayer_);
	}
	historyFeedbackOwned_ = false;
	historyFeedbackLayer_ = nullptr;
	historyFeedbackText_[0] = '\0';
}

bool LoopStationView::formatHistoryFeedback(char* text, size_t size) {
	using deluge::l10n::String;
	switch (historyPromptState_) {
	case HistoryPromptState::None:
		return false;
	case HistoryPromptState::Confirm: {
		char action[32]{};
		snprintf(action, sizeof(action),
		         deluge::l10n::get(historyTarget_.redo ? String::STRING_FOR_LOOP_STATION_REDO_PROMPT
		                                               : String::STRING_FOR_LOOP_STATION_UNDO_PROMPT),
		         static_cast<unsigned>(historyTarget_.slot) + 1);
		snprintf(text, size, "%s %s", action,
		         deluge::l10n::get(historyTarget_.redo ? String::STRING_FOR_LOOP_STATION_REDO_CONFIRM
		                                               : String::STRING_FOR_LOOP_STATION_UNDO_CONFIRM));
		return true;
	}
	case HistoryPromptState::Applying: {
		char track[16]{};
		snprintf(track, sizeof(track), deluge::l10n::get(String::STRING_FOR_LOOP_STATION_TRACK_NUMBER),
		         static_cast<unsigned>(historyTarget_.slot) + 1);
		snprintf(text, size, "%s %s", track,
		         deluge::l10n::get(historyTarget_.redo ? String::STRING_FOR_LOOP_STATION_REDOING
		                                               : String::STRING_FOR_LOOP_STATION_UNDOING));
		return true;
	}
	case HistoryPromptState::Result: {
		if (!validSlot(historyTarget_.slot)) {
			snprintf(text, size, "%s", deluge::l10n::get(String::STRING_FOR_LOOP_STATION_TRACK_UNAVAILABLE));
			return true;
		}
		char track[16]{};
		snprintf(track, sizeof(track), deluge::l10n::get(String::STRING_FOR_LOOP_STATION_TRACK_NUMBER),
		         static_cast<unsigned>(historyTarget_.slot) + 1);
		snprintf(text, size, "%s %s", track, historyTerminalText(historyStatus_, historyTarget_.redo));
		return true;
	}
	}
	return false;
}

bool LoopStationView::installHistoryFeedback(char const* text) {
	auto* layer = display->installScrollingOverlay(text, 0, 600, -1);
	if (!layer)
		return false;
	// The allocator may have yielded and re-entered during install. Never adopt a
	// layer for a prompt that was canceled, replaced, or is no longer ours.
	char current[64]{};
	if (getCurrentUI() != this || !formatHistoryFeedback(current, sizeof(current)) || strcmp(current, text) != 0) {
		display->removeLayer(layer);
		return false;
	}
	historyFeedbackLayer_ = layer;
	historyFeedbackOwned_ = true;
	snprintf(historyFeedbackText_, sizeof(historyFeedbackText_), "%s", text);
	return true;
}

bool LoopStationView::historyFeedbackVisible() {
	if (!display || display->haveOLED())
		return true; // the OLED renders the prompt from prompt state on every refresh
	return historyFeedbackOwned_ && historyFeedbackLayer_ && display->isLayerCurrentlyOnTop(historyFeedbackLayer_);
}

void LoopStationView::handleHistoryPress(bool redo) {
	auto* m = model();
	if (!m) {
		cancelHistoryConfirmation();
		return;
	}
	// While an accepted job is applying, presses are consumed without queuing.
	if (historyPromptState_ == HistoryPromptState::Applying)
		return;
	if (historyPromptState_ == HistoryPromptState::Confirm && historyTarget_.redo == redo) {
		// A destructive confirmation requires the feedback to be installed and
		// actually visible. A failed or covered overlay leaves the prompt armed so a
		// periodic refresh can retry; this press never submits blind.
		if (!historyFeedbackVisible())
			return;
		// Matching second press: submit exactly once. The model revalidates the
		// stable target under its serialized boundary and never queues.
		RestoreId id{};
		const auto status = m->requestHistoryNavigation(historyTarget_, id);
		if (status == Status::Ok) {
			historyId_ = id;
			historyPromptState_ = HistoryPromptState::Applying;
			historyStatus_ = Status::Ok;
		}
		else {
			historyId_ = {};
			historyPromptState_ = HistoryPromptState::Result;
			historyStatus_ = status;
		}
		refreshHistoryFeedback();
		return;
	}
	// From idle or a terminal result, or from the opposite shortcut, arm a fresh
	// confirmation. The opposite shortcut never confirms the previous operation.
	cancelHistoryConfirmation();
	const SlotId slot = m->engine().selected();
	if (!validSlot(slot)) {
		historyPromptState_ = HistoryPromptState::Result;
		historyStatus_ = Status::Invalid;
		refreshHistoryFeedback();
		return;
	}
	HistoryTarget target{};
	const auto status = m->historyTarget(slot, redo, target);
	if (status != Status::Ok) {
		historyTarget_.slot = slot;
		historyTarget_.redo = redo;
		historyPromptState_ = HistoryPromptState::Result;
		historyStatus_ = status;
		refreshHistoryFeedback();
		return;
	}
	historyTarget_ = target;
	historyPromptState_ = HistoryPromptState::Confirm;
	historyStatus_ = Status::Ok;
	refreshHistoryFeedback();
}

void LoopStationView::observeHistoryResult() {
	auto* m = model();
	if (!m) {
		cancelHistoryConfirmation();
		return;
	}
	if (historyPromptState_ == HistoryPromptState::Confirm) {
		// Stale identity (transport, rebind, session/phrase change, clear) cancels
		// the prompt with no audio change; added frames within one session do not.
		if (!m->historyTargetCurrent(historyTarget_))
			cancelHistoryConfirmation();
		else {
			// Identity is still current, but display eligibility can be lost to real
			// resource pressure (for example other-slot detached headroom). Replace
			// the stale instruction with an honest terminal reason instead of asking
			// for a confirmation that can no longer succeed.
			HistoryTarget eligible{};
			const auto status = m->historyTarget(historyTarget_.slot, historyTarget_.redo, eligible);
			if (status != Status::Ok) {
				historyPromptState_ = HistoryPromptState::Result;
				historyStatus_ = status;
			}
		}
		return;
	}
	if (historyPromptState_ != HistoryPromptState::Applying)
		return;
	// Bind the accepted job to the model that accepted it. A per-model request id
	// alone can be reused by a replacement model, and a replacement with no
	// matching result would otherwise leave the prompt stuck Applying.
	if (m->modelIdentity() != historyTarget_.modelIdentity) {
		cancelHistoryConfirmation();
		return;
	}
	const auto result = m->historyNavigationResult();
	if (result.id.generation != historyId_.generation || result.slot != historyTarget_.slot
	    || result.redo != historyTarget_.redo || result.pending())
		return;
	historyPromptState_ = HistoryPromptState::Result;
	historyStatus_ = result.status;
	historyTarget_.slot = result.slot;
	historyTarget_.redo = result.redo;
}

void LoopStationView::refreshHistoryFeedback() {
	if (!display || display->haveOLED())
		return; // OLED renders the persistent prompt from state on every refresh.
	char text[64]{};
	if (!formatHistoryFeedback(text, sizeof(text))) {
		clearHistoryFeedback();
		return;
	}
	if (getCurrentUI() != this)
		return; // another UI owns the display
	if (historyFeedbackOwned_) {
		// Never reinstall identical content: that would restart scrolling/blink.
		if (strcmp(historyFeedbackText_, text) == 0)
			return;
		// Content changed. Release the old owned overlay before installing the new
		// one so a transient allocation failure cannot leave both on screen.
		display->removeLayer(historyFeedbackLayer_);
		historyFeedbackOwned_ = false;
		historyFeedbackLayer_ = nullptr;
		historyFeedbackText_[0] = '\0';
	}
	// Failure is transient: ownership stays unset and the next refresh retries
	// once memory returns.
	(void)installHistoryFeedback(text);
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
	// Accepted pad presses are unrelated control actions; a release edge alone is not.
	if (velocity > 0)
		cancelHistoryConfirmation();
	auto* m = model();
	if (!m || !m->ready())
		return ActionResult::DEALT_WITH;
	// Shared exclusive modes own the input. Consume new presses, but still pass
	// releases through so a gesture accepted before the mode changed can finish.
	if (velocity > 0 && sharedExclusiveModeActive())
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
			updateModTarget();
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
	if (velocity > 0)
		updateModTarget();
	graphicsRoutine();
	return ActionResult::DEALT_WITH;
}

ActionResult LoopStationView::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	if (inCardRoutine)
		return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;

	const bool loopOwnsInput = !sharedExclusiveModeActive();
	auto* m = model();

	// A delivered BACK release always clears the down latch, whichever owner
	// receives the edge (a shared exclusive mode may take the release). A
	// card-routine-deferred edge returned above and is never counted twice.
	if (b == BACK && !on)
		historyBackDown_ = false;

	// Any unrelated accepted control press cancels a pending confirmation; a
	// release edge and SHIFT alone never do. A shared exclusive mode owns BACK
	// whenever it is active, so that press must cancel here and fall through.
	if (on && b != SHIFT && (b != BACK || !loopOwnsInput))
		cancelHistoryConfirmation();

	// While no shared exclusive mode is active, these loop shortcuts act on the
	// edge routed by the current mode and modifiers. Ownership is recomputed per
	// event, not remembered across mode, modifier, or current-UI changes.
	if (loopOwnsInput) {
		if (b == AFFECT_ENTIRE) {
			if (on && currentSong) {
				currentSong->affectEntire = !currentSong->affectEntire;
				updateModTarget();
			}
			return ActionResult::DEALT_WITH;
		}
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
					if (auto* clip = m->clip(slot)) {
						finishStutterBeforeTargetChange();
						sessionView.transitionToViewForClip(clip);
					}
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
			// Two released matching press edges confirm. A held BACK cannot
			// repeatedly confirm, and the modifier is sampled on each press. The
			// matching release was already handled above for every owner.
			if (on && !historyBackDown_) {
				historyBackDown_ = true;
				handleHistoryPress(Buttons::isShiftButtonPressed());
			}
			return ActionResult::DEALT_WITH;
		}
	}

	// PLAY, RECORD, SHIFT and every other ordinary control are the shared/native
	// handler's responsibility. Propagate its result so the native dispatcher sees
	// NOT_DEALT_WITH unchanged.
	const auto result = view.buttonAction(b, on, inCardRoutine);
	if ((b == SAVE || b == LOAD) && getCurrentUI() != this && m)
		m->cancelHeldPadGestures();
	return result;
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

	// A pending or terminal confirmation replaces the ordinary info row on both
	// display paths and persists across every graphics refresh.
	if (historyPromptState_ != HistoryPromptState::None) {
		using deluge::l10n::String;
		if (historyPromptState_ == HistoryPromptState::Confirm) {
			snprintf(line, sizeof(line),
			         deluge::l10n::get(historyTarget_.redo ? String::STRING_FOR_LOOP_STATION_REDO_PROMPT
			                                               : String::STRING_FOR_LOOP_STATION_UNDO_PROMPT),
			         static_cast<unsigned>(historyTarget_.slot) + 1);
			canvas.drawString(line, 0, 30, kTextSpacingX, kTextSizeYUpdated);
			canvas.drawString(deluge::l10n::get(historyTarget_.redo ? String::STRING_FOR_LOOP_STATION_REDO_CONFIRM
			                                                        : String::STRING_FOR_LOOP_STATION_UNDO_CONFIRM),
			                  0, 40, kTextSpacingX, kTextSmallSizeY);
		}
		else if (historyPromptState_ == HistoryPromptState::Applying) {
			canvas.drawString(deluge::l10n::get(historyTarget_.redo ? String::STRING_FOR_LOOP_STATION_REDOING
			                                                        : String::STRING_FOR_LOOP_STATION_UNDOING),
			                  0, 30, kTextSpacingX, kTextSizeYUpdated);
		}
		else {
			canvas.drawString(historyTerminalText(historyStatus_, historyTarget_.redo), 0, 30, kTextSpacingX,
			                  kTextSizeYUpdated);
		}
		return;
	}

	snprintf(line, sizeof(line), "IN: %s", sourceLabel(m->engine().settings().tracks[selected].source));
	canvas.drawString(line, 0, 30, kTextSpacingX, kTextSizeYUpdated);

	formatTrackLength(m->engine().state(selected), m->clock().tempoQ16(), line, sizeof(line));
	canvas.drawStringAlignRight(line, 30, kTextSpacingX, kTextSizeYUpdated);
}
void LoopStationView::selectEncoderAction(int8_t offset) {
	cancelHistoryConfirmation();
	if (sharedExclusiveModeActive())
		return;
	if (auto* m = model()) {
		int selected = m->engine().selected();
		if (!validSlot(selected))
			selected = 0;
		selected = (selected + offset + kTrackCount) % kTrackCount;
		const auto slot = static_cast<SlotId>(selected);
		if (prepareTrack(slot)) {
			(void)m->dispatch({CommandType::Select, slot});
			updateModTarget();
			graphicsRoutine();
		}
	}
}

// Modulation/encoder controls edit unrelated state. Cancel the prompt, then
// forward exactly the existing shared View behavior so nothing else changes.
void LoopStationView::modEncoderAction(int32_t whichModEncoder, int32_t offset) {
	cancelHistoryConfirmation();
	view.modEncoderAction(whichModEncoder, offset);
}

void LoopStationView::modButtonAction(uint8_t whichButton, bool on) {
	if (on)
		cancelHistoryConfirmation();
	view.modButtonAction(whichButton, on);
}

void LoopStationView::modEncoderButtonAction(uint8_t whichModEncoder, bool on) {
	if (on)
		cancelHistoryConfirmation();
	view.modEncoderButtonAction(whichModEncoder, on);
}

void LoopStationView::noteUnrelatedEncoderAction() {
	cancelHistoryConfirmation();
}
