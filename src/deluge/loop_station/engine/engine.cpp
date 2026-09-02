#include "loop_station/engine/engine.h"

#include <algorithm>
#include <limits>

namespace deluge::loop_station::core {
namespace {
constexpr uint32_t kDoublePressMs = 350;
}

Engine::Engine(SongModel& model, EngineDelegate& delegate) : model_(model), delegate_(delegate) {}

void Engine::restore(SlotId slot, int64_t length, int64_t origin, bool playing) {
	auto& r = track(slot);
	r = {};
	if (length <= 0) return;
	r.phraseLengthTicks = std::min(length, kMaxPhraseTicks);
	r.playbackOriginTick = origin;
	r.stableState = playing ? TrackState::PLAYING : TrackState::STOPPED;
	r.state = r.stableState;
}

int64_t Engine::nextBar(int64_t tick, int64_t barLength, bool includeBoundary) {
	if (barLength <= 0) return tick;
	auto phase = floorMod(tick, barLength);
	if (phase == 0) return includeBoundary ? tick : tick + barLength;
	return tick + barLength - phase;
}

int64_t Engine::smartStop(int64_t tick, int64_t barLength, int64_t eighthLength) {
	if (barLength <= 0) return tick;
	auto before = tick - floorMod(tick, barLength);
	auto after = before + barLength;
	auto nearest = tick - before <= after - tick ? before : after;
	return std::abs(nearest - tick) <= std::max<int64_t>(0, eighthLength) ? nearest : tick;
}

void Engine::centreDown(SlotId slot, TransportTime const& time, uint32_t gestureMs) {
	auto& r = track(slot);
	if (r.state == TrackState::STOPPED) return;
	bool doublePress = r.lastPressMs != UINT32_MAX && gestureMs - r.lastPressMs <= kDoublePressMs;
	r.lastPressMs = gestureMs;
	if (doublePress) secondary(slot, time);
	else {
		r.transaction = nextTransaction_++;
		primary(slot, time);
	}
}

void Engine::centreUp(SlotId slot, TransportTime const& time, uint32_t heldMs) {
	auto& r = track(slot);
	if (r.state == TrackState::STOPPED && heldMs < 2000) resume(slot, time);
}

void Engine::clearHold(SlotId slot) {
	auto& r = track(slot);
	if (r.state != TrackState::STOPPED || r.capturePhase != CapturePhase::NONE) return;
	delegate_.clearRevisions(slot);
	model_.tracks[indexOf(slot)].history.clear();
	r = {};
	setState(slot, TrackState::EMPTY);
}

void Engine::primary(SlotId slot, TransportTime const& time) {
	auto& r = track(slot);
	switch (r.state) {
	case TrackState::EMPTY: startInitial(slot, time); break;
	case TrackState::RECORDING: finishInitial(slot, time); break;
	case TrackState::PLAYING:
		if (model_.tracks[indexOf(slot)].settings.playbackMode == PlaybackMode::ONE_SHOT) {
			r.playbackOriginTick = time.performedTick;
			setState(slot, TrackState::PLAYING, 0);
		}
		else beginCapture(slot, true, time.performedTick, r.phraseLengthTicks);
		break;
	case TrackState::OVERDUBBING:
		r.capturePhase = CapturePhase::FINISHING_OVERDUB;
		r.playingWhileFinalizing = true;
		setState(slot, TrackState::PLAYING, logicalPhaseAt(time.performedTick, r.playbackOriginTick,
		                                                    r.phraseLengthTicks));
		break;
	case TrackState::ERROR:
		r.error = Error::NONE;
		r.state = r.stableState;
		delegate_.publishStableState(slot, r.state,
		                             logicalPhaseAt(time.performedTick, r.playbackOriginTick, r.phraseLengthTicks));
		break;
	default: break;
	}
}

void Engine::secondary(SlotId slot, TransportTime const& time) {
	auto& r = track(slot);
	if (r.capturePhase == CapturePhase::ACTIVE_OVERDUB || r.capturePhase == CapturePhase::FINISHING_OVERDUB) {
		delegate_.abortProvisionalCapture(slot, true);
		r.capturePhase = CapturePhase::NONE;
		r.playingWhileFinalizing = false;
		r.pending = {};
	}
	if (r.phraseLengthTicks > 0) requestStop(slot, time);
}

void Engine::startInitial(SlotId slot, TransportTime const& time) {
	auto mode = model_.global.recordQuantize;
	auto target = mode == RecordQuantizeMode::MEASURE
	                  ? nextBar(time.performedTick, time.barLength, true)
	                  : time.performedTick;
	if (target > time.performedTick) track(slot).pending = {PendingAction::START_CAPTURE, target};
	else beginCapture(slot, false, target, time.barLength);
}

void Engine::beginCapture(SlotId slot, bool overdub, int64_t tick, int64_t length) {
	auto& r = track(slot);
	if (!delegate_.preflight(slot, overdub) || !model_.tracks[indexOf(slot)].history.beginCommit()) {
		enterError(slot, Error::RESOURCE_LIMIT);
		return;
	}
	if (!delegate_.beginCapture(slot, overdub, tick, length)) {
		model_.tracks[indexOf(slot)].history.rejectCommit();
		enterError(slot, Error::ALLOCATION);
		return;
	}
	r.pending = {};
	r.captureStartTick = tick;
	if (overdub) {
		r.overdubStartTick = tick;
		r.overdubEndTick = tick + r.phraseLengthTicks;
		r.capturePhase = CapturePhase::ACTIVE_OVERDUB;
		setState(slot, TrackState::OVERDUBBING);
	}
	else {
		r.capturePhase = CapturePhase::INITIAL_TAKE;
		setState(slot, TrackState::RECORDING);
		auto bars = model_.tracks[indexOf(slot)].settings.durationBars;
		if (bars > 0) {
			auto duration = std::min<int64_t>(static_cast<int64_t>(bars) * std::max<int64_t>(1, length),
			                                  kMaxPhraseTicks);
			// length is supplied by the scheduler bridge as bar length for an initial take.
			if (duration > 0) r.pending = {PendingAction::FINISH_CAPTURE, tick + duration};
		}
	}
}

void Engine::finishInitial(SlotId slot, TransportTime const& time) {
	auto mode = model_.global.recordQuantize;
	auto target = time.performedTick;
	if (mode == RecordQuantizeMode::SMART) target = smartStop(target, time.barLength, time.eighthLength);
	else if (mode == RecordQuantizeMode::MEASURE) target = nextBar(target, time.barLength, false);
	if (target <= track(slot).captureStartTick) target = track(slot).captureStartTick + 1;
	if (target > time.performedTick) track(slot).pending = {PendingAction::FINISH_CAPTURE, target};
	else finishCapture(slot, false, target);
}

void Engine::finishCapture(SlotId slot, bool overdub, int64_t tick) {
	auto& r = track(slot);
	int64_t length = overdub ? r.phraseLengthTicks : tick - r.captureStartTick;
	length = std::clamp<int64_t>(length, 1, kMaxPhraseTicks);
	auto result = delegate_.endCapture(slot, overdub, length, !overdub);
	r.pending = {};
	if (!result.accepted) {
		model_.tracks[indexOf(slot)].history.rejectCommit();
		enterError(slot, Error::SD_CARD);
		return;
	}
	if (!overdub && result.actualPhraseLengthTicks > 0) length = result.actualPhraseLengthTicks;
	if (!overdub && r.phraseLengthTicks == 0) {
		r.phraseLengthTicks = length;
		r.playbackOriginTick = r.captureStartTick;
	}
	r.capturePhase = result.asynchronous ? CapturePhase::FINALIZING : CapturePhase::NONE;
	r.playingWhileFinalizing = result.asynchronous;
	r.stableState = TrackState::PLAYING;
	if (result.asynchronous) setState(slot, TrackState::COMMITTING);
	else commitCompleted(slot, true);
}

void Engine::commitCompleted(SlotId slot, bool success, Error error) {
	auto& r = track(slot);
	bool wasInitial = r.phraseLengthTicks > 0 && r.overdubEndTick == 0;
	if (!success) {
		model_.tracks[indexOf(slot)].history.rejectCommit();
		enterError(slot, error == Error::NONE ? Error::SD_CARD : error);
		return;
	}
	model_.tracks[indexOf(slot)].history.acceptCommit();
	r.capturePhase = CapturePhase::NONE;
	r.playingWhileFinalizing = false;
	r.overdubStartTick = r.overdubEndTick = 0;
	setState(slot, TrackState::PLAYING);
	if (wasInitial && model_.global.recordAction == RecordActionMode::RECORD_OVERDUB_PLAY) {
		r.dependentOverdub = true;
		// The exact boundary is the immutable first-take origin plus its length.
		r.pending = {PendingAction::START_CAPTURE, r.playbackOriginTick + r.phraseLengthTicks};
	}
}

void Engine::requestStop(SlotId slot, TransportTime const& time) {
	auto& r = track(slot);
	auto const& settings = model_.tracks[indexOf(slot)].settings;
	if (settings.stopMode == StopMode::LOOP_END) {
		auto phase = logicalPhaseAt(time.performedTick, r.playbackOriginTick, r.phraseLengthTicks);
		auto remaining = phase == 0 ? r.phraseLengthTicks : r.phraseLengthTicks - phase;
		r.pending = {PendingAction::STOP, time.performedTick + remaining};
	}
	else if (settings.stopMode == StopMode::FADE) {
		r.fadeEndTick = time.performedTick + settings.fadeBars * std::max<int64_t>(1, time.barLength);
		r.pending = {PendingAction::FINISH_FADE, r.fadeEndTick};
	}
	else stopNow(slot, time.performedTick);
}

void Engine::stopNow(SlotId slot, int64_t tick) {
	auto& r = track(slot);
	r.savedStopPhaseTicks = logicalPhaseAt(tick, r.playbackOriginTick, r.phraseLengthTicks);
	r.pending = {};
	setState(slot, TrackState::STOPPED, r.savedStopPhaseTicks);
}

void Engine::resume(SlotId slot, TransportTime const& time) {
	auto& r = track(slot);
	auto const& settings = model_.tracks[indexOf(slot)].settings;
	if (model_.global.startFromStopped == StartFromStoppedMode::FROM_START
	    || settings.playbackMode == PlaybackMode::ONE_SHOT) {
		r.playbackOriginTick = time.performedTick;
		setState(slot, TrackState::PLAYING, 0);
	}
	else if (!settings.loopSync) {
		r.playbackOriginTick = time.performedTick - r.savedStopPhaseTicks;
		setState(slot, TrackState::PLAYING, r.savedStopPhaseTicks);
	}
	else {
		auto target = nextBar(time.performedTick, time.barLength, true);
		r.pending = {PendingAction::RESUME, target};
	}
}

void Engine::process(TransportTime const& time) {
	for (size_t i = 0; i < kSlotCount; ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& r = runtime_[i];
		if ((r.capturePhase == CapturePhase::ACTIVE_OVERDUB || r.capturePhase == CapturePhase::FINISHING_OVERDUB)
		    && time.performedTick >= r.overdubEndTick) {
			finishCapture(slot, true, r.overdubEndTick);
		}
		if (r.pending.pending() && time.performedTick >= r.pending.tick) {
			auto action = r.pending.action;
			execute(slot, action, time);
		}
		if (r.state == TrackState::PLAYING
		    && model_.tracks[i].settings.playbackMode == PlaybackMode::ONE_SHOT
		    && r.phraseLengthTicks > 0 && time.performedTick >= r.playbackOriginTick + r.phraseLengthTicks) {
			stopNow(slot, r.playbackOriginTick + r.phraseLengthTicks);
		}
	}
}

void Engine::execute(SlotId slot, PendingAction action, TransportTime const& time) {
	auto& r = track(slot);
	auto eventTick = r.pending.tick;
	r.pending = {};
	switch (action) {
	case PendingAction::START_CAPTURE:
		beginCapture(slot, r.phraseLengthTicks > 0, eventTick,
		             r.phraseLengthTicks > 0 ? r.phraseLengthTicks : time.barLength);
		break;
	case PendingAction::FINISH_CAPTURE: finishCapture(slot, false, eventTick); break;
	case PendingAction::STOP:
	case PendingAction::FINISH_FADE: stopNow(slot, eventTick); break;
	case PendingAction::RESUME: setState(slot, TrackState::PLAYING, r.savedStopPhaseTicks); break;
	default: break;
	}
}

void Engine::fail(SlotId slot, Error error) { enterError(slot, error); }

void Engine::enterError(SlotId slot, Error error) {
	auto& r = track(slot);
	if (r.capturePhase != CapturePhase::NONE) {
		delegate_.abortProvisionalCapture(slot, r.capturePhase != CapturePhase::INITIAL_TAKE);
	}
	r.pending = {};
	r.capturePhase = CapturePhase::NONE;
	r.error = error;
	r.state = TrackState::ERROR;
	delegate_.publishStableState(slot, TrackState::ERROR, r.savedStopPhaseTicks);
}

bool Engine::navigate(SlotId slot, bool redo) {
	auto& r = track(slot);
	auto& history = model_.tracks[indexOf(slot)].history;
	if (isBusy(r) || (redo ? !history.canRedo() : !history.canUndo())) return false;
	bool targetHasPhrase = history.cursor + (redo ? 1 : -1) >= 0;
	if (!delegate_.navigateRevision(slot, redo, targetHasPhrase) || !history.navigate(redo)) return false;
	r.phraseLengthTicks = targetHasPhrase ? r.phraseLengthTicks : 0;
	setState(slot, targetHasPhrase ? TrackState::PLAYING : TrackState::EMPTY);
	return true;
}

bool Engine::undo(SlotId slot) { return navigate(slot, false); }
bool Engine::redo(SlotId slot) { return navigate(slot, true); }

void Engine::setState(SlotId slot, TrackState state, int64_t phase) {
	auto& r = track(slot);
	r.state = state;
	if (state == TrackState::EMPTY || state == TrackState::PLAYING || state == TrackState::STOPPED) r.stableState = state;
	delegate_.publishStableState(slot, state, phase);
}

bool Engine::isBusy(TrackRuntime const& r) const {
	return r.capturePhase != CapturePhase::NONE || r.state == TrackState::RECORDING
	       || r.state == TrackState::OVERDUBBING || r.state == TrackState::COMMITTING;
}

void Engine::requestTransportStop(TransportTime const& time) {
	transportStopRequested_ = true;
	requestExit(time);
}

void Engine::requestExit(TransportTime const& time) {
	exitRequested_ = true;
	for (size_t i = 0; i < kSlotCount; ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& r = runtime_[i];
		if (r.capturePhase == CapturePhase::INITIAL_TAKE) finishInitial(slot, time);
		else if (r.capturePhase == CapturePhase::ACTIVE_OVERDUB) r.capturePhase = CapturePhase::FINISHING_OVERDUB;
		else if (!isBusy(r)) r.pending = {};
	}
}

bool Engine::shouldKeepClockRunning() const {
	if (!transportStopRequested_) return false;
	return std::any_of(runtime_.begin(), runtime_.end(), [this](auto const& r) { return isBusy(r) || r.pending.pending(); });
}

bool Engine::exitReady() const {
	return exitRequested_ && std::none_of(runtime_.begin(), runtime_.end(), [this](auto const& r) {
		return isBusy(r) || r.pending.pending();
	});
}

int64_t Engine::ticksUntilNextEvent(int64_t now) const {
	auto result = std::numeric_limits<int64_t>::max();
	for (auto const& r : runtime_) {
		if (r.pending.pending()) result = std::min(result, std::max<int64_t>(0, r.pending.tick - now));
		if ((r.capturePhase == CapturePhase::ACTIVE_OVERDUB || r.capturePhase == CapturePhase::FINISHING_OVERDUB)
		    && r.overdubEndTick > 0) result = std::min(result, std::max<int64_t>(0, r.overdubEndTick - now));
	}
	return result;
}

} // namespace deluge::loop_station::core
