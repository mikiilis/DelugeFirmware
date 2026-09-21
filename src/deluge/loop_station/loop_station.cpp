/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "loop_station/loop_station.h"

#include <algorithm>

namespace deluge::loop_station {
namespace {
constexpr uint32_t kDoubleTapMs = 350;

struct CaptureFinishGuard {
	bool& active;
	bool previous;
	explicit CaptureFinishGuard(bool& flag) : active(flag), previous(flag) { active = true; }
	~CaptureFinishGuard() { active = previous; }
};

constexpr size_t indexOf(SlotId slot) {
	return static_cast<size_t>(slot);
}

void sanitizeTrackSettings(TrackSettings& settings, SlotId slot) {
	settings.source =
	    clampEnum<SourceType, SourceType::BALANCED>(static_cast<uint32_t>(settings.source), SourceType::LEFT);
	settings.progressColour = clampEnum<PadColour, PadColour::PINK>(static_cast<uint32_t>(settings.progressColour),
	                                                                defaultProgressColour(slot));
	if (isReservedStateColour(settings.progressColour)) {
		settings.progressColour = defaultProgressColour(slot);
	}
	settings.playback =
	    clampEnum<PlaybackMode, PlaybackMode::ONE_SHOT>(static_cast<uint32_t>(settings.playback), PlaybackMode::LOOP);
	settings.transpose = std::clamp<int16_t>(settings.transpose, -24, 24);
	settings.transposeCents = std::clamp<int8_t>(settings.transposeCents, -50, 50);
}

} // namespace

SongModel::SongModel() {
	for (size_t i = 0; i < tracks.size(); ++i) {
		tracks[i].settings.progressColour = defaultProgressColour(static_cast<SlotId>(i));
	}
}

void SongModel::sanitize() {
	global.recordAction = clampEnum<RecordAction, RecordAction::RECORD_PLAY_OVERDUB>(
	    static_cast<uint32_t>(global.recordAction), RecordAction::RECORD_PLAY_OVERDUB);
	for (auto* timing : {&global.startAction, &global.stopAction}) {
		auto fallback = timing == &global.startAction ? ActionMode::SYNCED : ActionMode::IMMEDIATE;
		timing->mode = clampEnum<ActionMode, ActionMode::SYNCED>(static_cast<uint32_t>(timing->mode), fallback);
		timing->division = clampEnum<ActionDivision, ActionDivision::EIGHTH>(static_cast<uint32_t>(timing->division),
		                                                                     ActionDivision::QUARTER);
	}
	global.dimBorderColour =
	    clampEnum<PadColour, PadColour::WHITE>(static_cast<uint32_t>(global.dimBorderColour), PadColour::BLUE);
	for (size_t i = 0; i < tracks.size(); ++i) {
		sanitizeTrackSettings(tracks[i].settings, static_cast<SlotId>(i));
	}
}

bool copyTrackConfiguration(SongModel& model, SlotId source, SlotId destination) {
	auto sourceIndex = indexOf(source);
	auto destinationIndex = indexOf(destination);
	if (sourceIndex >= model.tracks.size() || destinationIndex >= model.tracks.size()
	    || sourceIndex == destinationIndex) {
		return false;
	}
	model.tracks[destinationIndex].settings = model.tracks[sourceIndex].settings;
	sanitizeTrackSettings(model.tracks[destinationIndex].settings, destination);
	return true;
}

int32_t readGlobalParameter(SongModel const& model, GlobalParameter parameter) {
	if (parameter == GlobalParameter::RECORD_ACTION)
		return static_cast<int32_t>(model.global.recordAction);
	if (parameter == GlobalParameter::DIM_BORDER_COLOUR)
		return static_cast<int32_t>(model.global.dimBorderColour);
	auto const& timing = (parameter == GlobalParameter::START_ACTION || parameter == GlobalParameter::START_INTERVAL)
	                         ? model.global.startAction
	                         : model.global.stopAction;
	return parameter == GlobalParameter::START_INTERVAL || parameter == GlobalParameter::STOP_INTERVAL
	           ? static_cast<int32_t>(timing.division) + (timing.triplet ? 4 : 0)
	           : static_cast<int32_t>(timing.mode);
}

void writeGlobalParameter(SongModel& model, GlobalParameter parameter, int32_t value) {
	if (parameter == GlobalParameter::RECORD_ACTION) {
		model.global.recordAction = clampEnum<RecordAction, RecordAction::RECORD_PLAY_OVERDUB>(
		    static_cast<uint32_t>(value), RecordAction::RECORD_PLAY_OVERDUB);
		return;
	}
	if (parameter == GlobalParameter::DIM_BORDER_COLOUR) {
		model.global.dimBorderColour =
		    clampEnum<PadColour, PadColour::WHITE>(static_cast<uint32_t>(value), PadColour::BLUE);
		return;
	}
	bool start = parameter == GlobalParameter::START_ACTION || parameter == GlobalParameter::START_INTERVAL;
	auto& timing = start ? model.global.startAction : model.global.stopAction;
	if (parameter == GlobalParameter::START_INTERVAL || parameter == GlobalParameter::STOP_INTERVAL) {
		if (value < 0 || value > 7)
			value = 2;
		timing.division = static_cast<ActionDivision>(value % 4);
		timing.triplet = value >= 4;
	}
	else {
		timing.mode = clampEnum<ActionMode, ActionMode::SYNCED>(static_cast<uint32_t>(value),
		                                                        start ? ActionMode::SYNCED : ActionMode::IMMEDIATE);
	}
}

int32_t readTrackParameter(SongModel const& model, SlotId slot, TrackParameter parameter) {
	auto const& settings = model.tracks[indexOf(slot)].settings;
	switch (parameter) {
	case TrackParameter::SOURCE:
		return static_cast<int32_t>(settings.source);
	case TrackParameter::SOURCE_TRACK:
		return settings.sourceTrackId;
	case TrackParameter::MONITORING:
		return settings.monitoring;
	case TrackParameter::DURATION_BARS:
		return settings.durationBars;
	case TrackParameter::PROGRESS_COLOUR:
		return static_cast<int32_t>(settings.progressColour);
	case TrackParameter::PLAYBACK:
		return static_cast<int32_t>(settings.playback);
	case TrackParameter::TRANSPOSE:
		return settings.transpose * 100 + settings.transposeCents;
	}
	return 0;
}

void writeTrackParameter(SongModel& model, SlotId slot, TrackParameter parameter, int32_t value) {
	auto& settings = model.tracks[indexOf(slot)].settings;
	switch (parameter) {
	case TrackParameter::SOURCE:
		settings.source = clampEnum<SourceType, SourceType::BALANCED>(static_cast<uint32_t>(value), SourceType::LEFT);
		break;
	case TrackParameter::SOURCE_TRACK:
		settings.sourceTrackId = std::max<int32_t>(value, 0);
		break;
	case TrackParameter::MONITORING:
		settings.monitoring = value != 0;
		break;
	case TrackParameter::DURATION_BARS:
		settings.durationBars = static_cast<uint16_t>(std::clamp<int32_t>(value, 0, UINT16_MAX));
		break;
	case TrackParameter::PROGRESS_COLOUR:
		settings.progressColour =
		    clampEnum<PadColour, PadColour::PINK>(static_cast<uint32_t>(value), defaultProgressColour(slot));
		if (isReservedStateColour(settings.progressColour)) {
			settings.progressColour = defaultProgressColour(slot);
		}
		break;
	case TrackParameter::PLAYBACK:
		settings.playback =
		    clampEnum<PlaybackMode, PlaybackMode::ONE_SHOT>(static_cast<uint32_t>(value), PlaybackMode::LOOP);
		break;
	case TrackParameter::TRANSPOSE: {
		int32_t valueInCents = std::clamp<int32_t>(value, -2450, 2450);
		settings.transpose = static_cast<int16_t>(valueInCents / 100);
		settings.transposeCents = static_cast<int8_t>(valueInCents % 100);
	} break;
	}
}

Engine::Engine(SongModel& model, EngineDelegate* delegate) : model_(model), delegate_(delegate) {
	model_.sanitize();
}

void Engine::resetRuntime() {
	runtime_ = {};
	exiting_ = false;
	nextRevision_ = 1;
	for (size_t i = 0; i < runtime_.size(); ++i) {
		setPlaybackGain(static_cast<SlotId>(i), 1 << 24, true);
	}
}

void Engine::restoreBoundPhrase(SlotId slot, uint32_t phraseLength, bool playing, bool hasPhrase, uint32_t historySize,
                                int32_t historyCursor) {
	auto& runtime = track(slot);
	runtime = {};
	runtime.phraseLength = phraseLength;
	runtime.currentRevision = hasPhrase ? nextRevision_++ : 0;
	runtime.historySize = historySize;
	runtime.historyCursor = std::clamp<int32_t>(historyCursor, -1, static_cast<int32_t>(historySize) - 1);
	runtime.state = hasPhrase ? (playing ? TrackState::PLAYING : TrackState::STOPPED) : TrackState::EMPTY;
	setPlaybackGain(slot, playing ? (1 << 24) : 0, true);
	refreshHistoryAvailability(slot);
}

TrackRuntime const& Engine::track(SlotId slot) const {
	return runtime_[indexOf(slot)];
}

TrackRuntime& Engine::track(SlotId slot) {
	return runtime_[indexOf(slot)];
}

uint64_t Engine::actionPositionQ16(Transport const& transport, ActionTiming timing) {
	uint64_t position = (static_cast<uint64_t>(transport.tick) << 16) | transport.fractionQ16;
	if (timing.mode == ActionMode::IMMEDIATE || !transport.barLength)
		return position;
	// Sixths retain exact half beats and triplets, including odd tick resolutions.
	uint64_t pulse = std::max<uint32_t>(1, transport.barLength / 4) * (timing.triplet ? 4ull : 6ull);
	uint32_t count = 1;
	switch (timing.division) {
	case ActionDivision::BAR:
		count = 4;
		break;
	case ActionDivision::HALF:
		count = 2;
		break;
	case ActionDivision::QUARTER:
		break;
	case ActionDivision::EIGHTH:
		pulse /= 2;
		break;
	}
	uint64_t offset = static_cast<uint64_t>(transport.metronomeOffset) << 16;
	uint64_t target = (((position + offset) * 6 / (pulse << 16)) + count) * pulse;
	return std::min<uint64_t>(((target << 16) + 5) / 6 - offset, static_cast<uint64_t>(UINT32_MAX) << 16);
}

uint32_t Engine::actionTick(Transport const& transport, ActionTiming timing) {
	return static_cast<uint32_t>(std::min<uint64_t>((actionPositionQ16(transport, timing) + 65535) >> 16, UINT32_MAX));
}

void Engine::requestAction(SlotId slot, PendingAction action, Transport const& transport, ActionTiming timing) {
	uint64_t target = actionPositionQ16(transport, timing);
	if (timing.mode == ActionMode::IMMEDIATE || !transport.barLength)
		execute(slot, action, transport, transport.tick);
	else
		schedule(slot, action, static_cast<uint32_t>((target + 65535) >> 16),
		         static_cast<uint32_t>((target + 65535) >> 16), target);
}

bool Engine::isDoubleTap(TrackRuntime& runtime, uint32_t gestureTimeMs) {
	bool result = runtime.lastTapTimeMs != std::numeric_limits<uint32_t>::max()
	              && gestureTimeMs - runtime.lastTapTimeMs <= kDoubleTapMs;
	runtime.lastTapTimeMs = gestureTimeMs;
	return result;
}

void Engine::centrePressed(SlotId slot, Transport const& transport, uint32_t gestureTimeMs) {
	auto& runtime = track(slot);
	if (runtime.finishingCapture) {
		return;
	}
	if (runtime.state == TrackState::ERROR) {
		runtime.error = Error::NONE;
		transition(slot, runtime.stateBeforeCommit);
		return;
	}
	if (runtime.state == TrackState::COMMITTING) {
		return;
	}
	// A second gesture cancels a user intention, never the automatic record limit.
	if (runtime.event.pending()) {
		runtime.event = {};
		runtime.stopAfterCapture = false;
		runtime.lastTapTimeMs = UINT32_MAX;
		return;
	}
	bool doubleTap = isDoubleTap(runtime, gestureTimeMs);
	if (doubleTap && runtime.state == TrackState::OVERDUBBING) {
		if (delegate_)
			delegate_->abortCapture(slot, true);
		runtime.finishingOverdub = false;
		runtime.overdubEndTick = 0;
		transition(slot, TrackState::PLAYING);
		stop(slot, transport);
		return;
	}

	switch (runtime.state) {
	case TrackState::EMPTY:
		requestAction(slot, PendingAction::START_RECORDING, transport, model_.global.startAction);
		break;
	case TrackState::RECORDING:
		requestAction(slot, PendingAction::FINISH_RECORDING, transport, model_.global.stopAction);
		break;
	case TrackState::PLAYING:
		if (doubleTap)
			stop(slot, transport);
		else if (model_.tracks[indexOf(slot)].settings.playback == PlaybackMode::ONE_SHOT)
			requestAction(slot, PendingAction::RETRIGGER, transport, model_.global.startAction);
		else if (!runtime.finishingOverdub)
			execute(slot, PendingAction::START_OVERDUB, transport, transport.tick);
		break;
	case TrackState::OVERDUBBING:
		// The input gate closes now; materialization of untouched audio may finish
		// in the background, independently of the visible playback state.
		runtime.finishingOverdub = true;
		if (delegate_)
			delegate_->endOverdubInput(slot);
		transition(slot, TrackState::PLAYING);
		serviceAudioCompletion(transport);
		break;
	case TrackState::STOPPED:
		if (runtime.finishingOverdub)
			break;
		requestAction(slot, PendingAction::RETRIGGER, transport, model_.global.startAction);
		break;
	case TrackState::COMMITTING:
	case TrackState::ERROR:
		break;
	}
}

void Engine::schedule(SlotId slot, PendingAction action, uint32_t atTick, uint32_t requestedTick,
                      uint64_t positionQ16) {
	track(slot).event = {.action = action, .tick = atTick, .requestedTick = requestedTick, .positionQ16 = positionQ16};
}

void Engine::execute(SlotId slot, PendingAction action, Transport const& transport, uint32_t requestedTick) {
	switch (action) {
	case PendingAction::START_RECORDING:
		beginCapture(slot, false, transport.tick, transport);
		break;
	case PendingAction::FINISH_RECORDING:
		finishCapture(slot, false, transport.tick, requestedTick);
		break;
	case PendingAction::START_OVERDUB:
		beginCapture(slot, true, transport.tick, transport);
		break;
	case PendingAction::FINISH_OVERDUB:
		finishCapture(slot, true, transport.tick, requestedTick);
		break;
	case PendingAction::STOP:
		stop(slot, transport, true);
		break;
	case PendingAction::RETRIGGER:
		beginPlayback(slot, transport);
		break;
	case PendingAction::RESUME:
		beginPlayback(slot, transport);
		break;
	case PendingAction::NONE:
		break;
	}
}

void Engine::startAll(Transport const& transport) {
	for (size_t i = 0; i < runtime_.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& runtime = runtime_[i];
		if (runtime.finishingCapture || runtime.state != TrackState::STOPPED || runtime.event.pending()
		    || runtime.finishingOverdub)
			continue;
		requestAction(slot, PendingAction::RETRIGGER, transport, model_.global.startAction);
	}
}

void Engine::stopAll(Transport const& transport) {
	for (size_t i = 0; i < runtime_.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& runtime = runtime_[i];
		if (runtime.finishingCapture)
			continue;
		if (runtime.event.action == PendingAction::START_RECORDING
		    || runtime.event.action == PendingAction::RETRIGGER) {
			runtime.event = {};
		}
		if (runtime.state == TrackState::COMMITTING) {
			runtime.stopAfterCapture = true;
			runtime.stateBeforeCommit = TrackState::STOPPED;
			setPlaybackGain(slot, 0, true);
			continue;
		}
		if (runtime.state == TrackState::RECORDING) {
			runtime.stopAfterCapture = true;
			if (!runtime.event.pending()) {
				requestAction(slot, PendingAction::FINISH_RECORDING, transport, model_.global.stopAction);
			}
		}
		else if (runtime.state == TrackState::PLAYING || runtime.state == TrackState::OVERDUBBING) {
			if (runtime.state == TrackState::OVERDUBBING) {
				runtime.finishingOverdub = true;
				if (delegate_)
					delegate_->endOverdubInput(slot);
				transition(slot, TrackState::PLAYING);
			}
			if (!runtime.event.pending())
				stop(slot, transport);
		}
	}
}

bool Engine::canStartAll() const {
	return std::any_of(runtime_.begin(), runtime_.end(), [](TrackRuntime const& r) {
		return r.state == TrackState::STOPPED && !r.event.pending() && !r.finishingOverdub;
	});
}

bool Engine::canStopAll() const {
	return std::any_of(runtime_.begin(), runtime_.end(), [](TrackRuntime const& r) {
		return r.state == TrackState::PLAYING || r.state == TrackState::RECORDING || r.state == TrackState::OVERDUBBING
		       || r.state == TrackState::COMMITTING || r.event.pending();
	});
}

void Engine::beginCapture(SlotId slot, bool overdub, uint32_t tick, Transport const& transport) {
	auto& runtime = track(slot);
	if (runtime.finishingCapture || runtime.handlingFailure || exiting_)
		return;
	// Allocation, recorder setup and notifications may all re-enter the UI.
	// Reset stop intent before yielding so transportStopped can set it again.
	runtime.stopAfterCapture = false;
	{
		CaptureFinishGuard guard(runtime.finishingCapture);
		if (delegate_ && (!delegate_->preflight(slot, overdub) || !delegate_->beginCapture(slot, overdub, tick))) {
			fail(slot, Error::RESOURCE_LIMIT);
			return;
		}
		// EMPTY may have been reached from a stopped phrase whose persistent
		// AudioOutput gain is still zero. Prime every fresh take at unity.
		if (!overdub) {
			setPlaybackGain(slot, 1 << 24, true);
		}
		runtime.captureStartTick = tick;
		runtime.automaticEndTick = 0;
		runtime.automaticEndPositionQ16 = 0;
		auto bars = model_.tracks[indexOf(slot)].settings.durationBars;
		if (!overdub && bars != kManualDuration && transport.barLength) {
			runtime.automaticEndPositionQ16 = std::min<uint64_t>(
			    ((static_cast<uint64_t>(tick) + static_cast<uint64_t>(bars) * transport.barLength) << 16)
			        + transport.fractionQ16,
			    static_cast<uint64_t>(kMaxLoopSequenceLength) << 16);
			runtime.automaticEndTick = (runtime.automaticEndPositionQ16 + 65535) >> 16;
		}
		runtime.event = {};
		runtime.overdubStartTick = 0;
		runtime.overdubEndTick = 0;
		runtime.finishingOverdub = false;
		runtime.error = Error::NONE;
		if (overdub) {
			runtime.overdubStartTick = tick;
		}
		transition(slot, overdub ? TrackState::OVERDUBBING : TrackState::RECORDING);
	}
	if (runtime.stopAfterCapture) {
		runtime.transportStopPending = false;
		if (overdub) {
			runtime.finishingOverdub = true;
			if (delegate_)
				delegate_->endOverdubInput(slot);
			stop(slot, transport, true);
		}
		else
			finishCapture(slot, false, tick, tick);
	}
	if (exiting_)
		requestExit(transport);
}

void Engine::finishCapture(SlotId slot, bool overdub, uint32_t tick, uint32_t requestedTick) {
	auto& runtime = track(slot);
	if (runtime.finishingCapture || runtime.state == TrackState::COMMITTING) {
		return;
	}
	CaptureFinishGuard guard(runtime.finishingCapture);
	runtime.overdubEndTick = 0;
	runtime.automaticEndTick = 0;
	runtime.automaticEndPositionQ16 = 0;
	uint32_t endTick = requestedTick;
	if (endTick <= runtime.captureStartTick) {
		endTick = tick;
	}
	uint32_t length =
	    overdub ? runtime.phraseLength : (endTick > runtime.captureStartTick ? endTick - runtime.captureStartTick : 1);
	if (length == 0) {
		if (delegate_) {
			delegate_->abortCapture(slot, overdub);
		}
		transition(slot, overdub ? TrackState::PLAYING : TrackState::EMPTY);
		return;
	}
	if (overdub) {
		runtime.overdubEndTick = tick;
	}
	// The cumulative sample retains its original frame length; initial takes keep every captured frame.
	bool crop = false;
	auto finish = delegate_ ? delegate_->finishCapture(slot, overdub, length, crop) : CaptureFinish{};
	if (!finish.accepted) {
		fail(slot, finish.error == Error::NONE ? Error::SD_CARD : finish.error);
		return;
	}
	uint32_t committedLength = finish.committedLength ? finish.committedLength : length;
	if (runtime.event.action == PendingAction::FINISH_RECORDING
	    || runtime.event.action == PendingAction::FINISH_OVERDUB)
		runtime.event = {};
	if (finish.asynchronous) {
		runtime.committingLength = committedLength;
		runtime.committingOverdub = overdub;
		runtime.finishingOverdub = false;
		if (!overdub) {
			runtime.phraseLength = committedLength;
			runtime.playbackStartTick = tick;
			setPlaybackGain(slot, 1 << 24, true);
		}
		auto destination = runtime.stopAfterCapture ? TrackState::STOPPED : TrackState::PLAYING;
		if (destination == TrackState::STOPPED)
			setPlaybackGain(slot, 0, true);
		transition(slot, destination);
		if (!overdub && !runtime.stopAfterCapture && !exiting_
		    && model_.global.recordAction == RecordAction::RECORD_OVERDUB_PLAY
		    && model_.tracks[indexOf(slot)].settings.playback == PlaybackMode::LOOP) {
			if (delegate_ && delegate_->preflight(slot, true) && delegate_->beginCapture(slot, true, tick)) {
				runtime.captureStartTick = runtime.overdubStartTick = tick;
				transition(slot, TrackState::OVERDUBBING);
			}
			else {
				fail(slot, Error::RESOURCE_LIMIT);
			}
		}
	}
	else {
		completeCapture(slot, overdub, committedLength);
	}
}

void Engine::completeCapture(SlotId slot, bool overdub, uint32_t length, uint32_t completionTick) {
	auto& runtime = track(slot);
	if (overdub) {
		runtime.currentRevision = nextRevision_++;
		runtime.finishingOverdub = false;
		runtime.overdubStartTick = 0;
		runtime.overdubEndTick = 0;
	}
	else {
		runtime.currentRevision = nextRevision_++;
		runtime.phraseLength = length;
		runtime.playbackStartTick = model_.tracks[indexOf(slot)].settings.playback == PlaybackMode::ONE_SHOT
		                                ? runtime.captureStartTick + length
		                                : runtime.captureStartTick;
		// Capture completion enters PLAYING / OVERDUBBING without passing through
		// beginPlayback(), so reassert unity at this audible boundary too.
		setPlaybackGain(slot, 1 << 24, true);
	}
	// A successful commit truncates any redo branch and appends exactly one new
	// cumulative revision. Failed captures never reach this method.
	runtime.historySize = static_cast<uint32_t>(runtime.historyCursor + 2);
	runtime.historyCursor++;
	refreshHistoryAvailability(slot);
	TrackState destination = runtime.stopAfterCapture ? TrackState::STOPPED : TrackState::PLAYING;
	if (!runtime.stopAfterCapture && !exiting_ && !overdub
	    && model_.global.recordAction == RecordAction::RECORD_OVERDUB_PLAY
	    && model_.tracks[indexOf(slot)].settings.playback == PlaybackMode::LOOP) {
		// REC→DUB starts continuous overdub at the fresh phrase boundary.
		destination = TrackState::OVERDUBBING;
		// Synchronous delegates enter the next performance state at this boundary.
		runtime.captureStartTick = completionTick ? completionTick : runtime.captureStartTick + length;
		if (delegate_
		    && (!delegate_->preflight(slot, true) || !delegate_->beginCapture(slot, true, runtime.captureStartTick))) {
			fail(slot, Error::RESOURCE_LIMIT);
			return;
		}
		else {
			runtime.overdubStartTick = runtime.captureStartTick;
		}
	}
	if (destination == TrackState::STOPPED)
		setPlaybackGain(slot, 0, true);
	transition(slot, destination);
}

void Engine::stop(SlotId slot, Transport const& transport, bool forceImmediate) {
	auto& runtime = track(slot);
	if (runtime.finishingCapture)
		return;
	if (!forceImmediate) {
		requestAction(slot, PendingAction::STOP, transport, model_.global.stopAction);
		return;
	}
	runtime.event = {};
	runtime.stopAfterCapture = true;
	setPlaybackGain(slot, 0, true);
	transition(slot, runtime.phraseLength ? TrackState::STOPPED : TrackState::EMPTY);
}

void Engine::beginPlayback(SlotId slot, Transport const& transport) {
	auto& runtime = track(slot);
	runtime.event = {};
	runtime.stopAfterCapture = false;
	runtime.playbackStartTick = transport.tick;
	setPlaybackGain(slot, 1 << 24, true);
	transition(slot, TrackState::PLAYING);
	if (delegate_)
		delegate_->restartPlayback(slot);
}

void Engine::setPlaybackGain(SlotId slot, int32_t gain, bool forceNotify) {
	auto& runtime = track(slot);
	bool changed = gain != runtime.playbackGain;
	runtime.playbackGain = gain;
	if (delegate_ && (changed || forceNotify)) {
		delegate_->playbackGainChanged(slot, gain);
	}
}

void Engine::processTick(Transport const& transport) {
	for (size_t i = 0; i < runtime_.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& runtime = runtime_[i];
		if (runtime.finishingCapture || runtime.state == TrackState::COMMITTING) {
			continue;
		}
		if (runtime.event.pending()
		    && ((static_cast<uint64_t>(transport.tick) << 16) | transport.fractionQ16) >= runtime.event.deadline()) {
			auto event = runtime.event;
			runtime.event = {};
			execute(slot, event.action, transport, event.requestedTick);
		}

		if (runtime.state == TrackState::RECORDING && runtime.automaticEndTick
		    && ((static_cast<uint64_t>(transport.tick) << 16) | transport.fractionQ16)
		           >= runtime.automaticEndPositionQ16) {
			auto end = runtime.automaticEndTick;
			finishCapture(slot, false, end, end);
		}
	}
}

void Engine::serviceAudioCompletion(Transport const& transport) {
	if (exiting_)
		requestExit(transport);
	for (size_t i = 0; i < runtime_.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& r = runtime_[i];
		if (r.finishingCapture)
			continue;
		if (r.transportStopPending) {
			r.transportStopPending = false;
			if (r.state == TrackState::RECORDING)
				finishCapture(slot, false, transport.tick, transport.tick);
			else if (r.state == TrackState::OVERDUBBING) {
				CaptureFinishGuard guard(r.finishingCapture);
				r.finishingOverdub = true;
				if (delegate_)
					delegate_->endOverdubInput(slot);
			}
			if (r.phraseLength)
				stop(slot, transport, true);
		}
		if (r.finishingOverdub && !r.committingLength && (!delegate_ || delegate_->overdubReady(slot))) {
			finishCapture(slot, true, transport.tick, transport.tick);
		}
		if (r.state == TrackState::PLAYING && !r.finishingOverdub && !r.committingLength
		    && model_.tracks[i].settings.playback == PlaybackMode::ONE_SHOT && delegate_
		    && delegate_->playbackEnded(slot))
			stop(slot, transport, true);
	}
}

bool Engine::requestExit(Transport const& transport) {
	exiting_ = true;
	for (size_t i = 0; i < runtime_.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& runtime = runtime_[i];
		if (runtime.finishingCapture || runtime.state == TrackState::COMMITTING)
			continue;
		if (runtime.state == TrackState::RECORDING && !runtime.event.pending()) {
			requestAction(slot, PendingAction::FINISH_RECORDING, transport, model_.global.stopAction);
		}
		else if (runtime.state == TrackState::OVERDUBBING) {
			runtime.finishingOverdub = true;
			if (delegate_)
				delegate_->endOverdubInput(slot);
			transition(slot, TrackState::PLAYING);
		}
		else if (runtime.event.action == PendingAction::START_RECORDING
		         || runtime.event.action == PendingAction::RETRIGGER)
			runtime.event = {};
	}
	return exitReady();
}

bool Engine::exitReady() const {
	if (!exiting_) {
		return false;
	}
	for (auto const& runtime : runtime_) {
		if (runtime.finishingCapture || runtime.handlingFailure || runtime.transportStopPending
		    || runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING
		    || runtime.finishingOverdub || runtime.committingLength || runtime.state == TrackState::COMMITTING
		    || runtime.event.pending()) {
			return false;
		}
	}
	return true;
}

bool Engine::anyCapturing() const {
	return std::any_of(runtime_.begin(), runtime_.end(), [](TrackRuntime const& runtime) {
		return runtime.finishingCapture || runtime.handlingFailure || runtime.transportStopPending
		       || runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING
		       || runtime.finishingOverdub || runtime.committingLength || runtime.state == TrackState::COMMITTING;
	});
}

uint64_t Engine::nextActionPositionQ16() const {
	uint64_t result = UINT64_MAX;
	for (auto const& r : runtime_) {
		if (r.finishingCapture || r.state == TrackState::COMMITTING)
			continue;
		if (r.event.pending())
			result = std::min(result, r.event.deadline());
		if (r.automaticEndTick)
			result = std::min(result, r.automaticEndPositionQ16);
	}
	return result;
}

void Engine::transportStopped(Transport const& transport) {
	for (size_t i = 0; i < runtime_.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& r = runtime_[i];
		r.event = {};
		r.stopAfterCapture = true;
		if (r.finishingCapture) {
			r.transportStopPending = true;
			continue;
		}
		if (r.state == TrackState::RECORDING)
			finishCapture(slot, false, transport.tick, transport.tick);
		else if (r.state == TrackState::OVERDUBBING) {
			r.finishingOverdub = true;
			if (delegate_)
				delegate_->endOverdubInput(slot);
		}
		if (r.phraseLength)
			stop(slot, transport, true);
	}
}

uint32_t Engine::ticksUntilNextEvent(uint32_t currentTick) const {
	uint32_t result = UINT32_MAX;
	for (auto const& r : runtime_) {
		if (r.finishingCapture || r.state == TrackState::COMMITTING)
			continue;
		auto consider = [&](uint32_t tick) {
			if (tick)
				result = std::min(result, tick > currentTick ? tick - currentTick : 0);
		};
		if (r.event.pending())
			consider(r.event.tick);
		consider(r.automaticEndTick);
	}
	return result;
}

void Engine::clearHeld(SlotId slot) {
	auto& runtime = track(slot);
	if (runtime.finishingCapture || runtime.state != TrackState::STOPPED || runtime.committingLength
	    || runtime.finishingOverdub) {
		return;
	}
	runtime = {};
	// TrackRuntime defaults to unity, but AudioOutput persists independently
	// and may still contain the zero sent when this phrase stopped.
	setPlaybackGain(slot, 1 << 24, true);
	if (delegate_) {
		delegate_->phraseCleared(slot);
	}
	transition(slot, TrackState::EMPTY);
}

bool Engine::undo(SlotId slot) {
	return navigateHistory(slot, false);
}

bool Engine::redo(SlotId slot) {
	return navigateHistory(slot, true);
}

bool Engine::navigateHistory(SlotId slot, bool redo) {
	auto& runtime = track(slot);
	if (runtime.finishingCapture || runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING
	    || runtime.committingLength || runtime.finishingOverdub || runtime.state == TrackState::COMMITTING
	    || (redo ? !runtime.redoAvailable : !runtime.undoAvailable)) {
		return false;
	}
	int32_t targetCursor = runtime.historyCursor + (redo ? 1 : -1);
	bool targetHasPhrase = targetCursor >= 0;
	if (delegate_ && !delegate_->navigateHistory(slot, redo, targetHasPhrase)) {
		return false;
	}
	runtime.historyCursor = targetCursor;
	runtime.currentRevision = targetHasPhrase ? nextRevision_++ : 0;
	refreshHistoryAvailability(slot);
	// Navigation can leave STOPPED and enter either PLAYING or the undoable
	// EMPTY state. Unity makes the selected revision and the next take audible.
	setPlaybackGain(slot, 1 << 24, true);
	transition(slot, targetHasPhrase ? TrackState::PLAYING : TrackState::EMPTY);
	return true;
}

void Engine::refreshHistoryAvailability(SlotId slot) {
	auto& runtime = track(slot);
	runtime.undoAvailable = runtime.historyCursor >= 0;
	runtime.redoAvailable = runtime.historyCursor + 1 < static_cast<int32_t>(runtime.historySize);
}

void Engine::notifyCommitComplete(SlotId slot, bool success, Error error, uint32_t completionTick) {
	auto& runtime = track(slot);
	if (!runtime.committingLength)
		return;
	runtime.committingLength = 0;
	if (!success) {
		fail(slot, error == Error::NONE ? Error::SD_CARD : error);
		return;
	}
	runtime.currentRevision = nextRevision_++;
	runtime.historySize = static_cast<uint32_t>(runtime.historyCursor + 2);
	runtime.historyCursor++;
	refreshHistoryAvailability(slot);
}

void Engine::notifyFailure(SlotId slot, Error error) {
	fail(slot, error);
}

void Engine::transition(SlotId slot, TrackState state) {
	auto& runtime = track(slot);
	runtime.state = state;
	if (delegate_) {
		delegate_->stateChanged(slot, state);
	}
}

void Engine::fail(SlotId slot, Error error) {
	auto& runtime = track(slot);
	if (runtime.handlingFailure)
		return;
	CaptureFinishGuard failureGuard(runtime.handlingFailure);
	CaptureFinishGuard captureGuard(runtime.finishingCapture);
	bool hadPhrase = runtime.currentRevision != 0 || runtime.committingLength != 0;
	if (runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING
	    || runtime.finishingOverdub) {
		if (delegate_) {
			delegate_->abortCapture(slot, runtime.state == TrackState::OVERDUBBING || runtime.finishingOverdub);
		}
	}
	runtime.event = {};
	runtime.overdubEndTick = 0;
	runtime.finishingOverdub = false;
	// A preceding take may still be publishing while the next overdub fails.
	// Its completion owns committingLength and must remain observable.
	runtime.automaticEndTick = 0;
	runtime.automaticEndPositionQ16 = 0;
	runtime.error = error;
	transition(slot, TrackState::ERROR);
	// Error is a visual latch; a subsequent render/ack can restore the playable state.
	runtime.stateBeforeCommit =
	    hadPhrase ? (runtime.stopAfterCapture ? TrackState::STOPPED : TrackState::PLAYING) : TrackState::EMPTY;
}

uint8_t Geometry::progressPadCount(uint32_t position, uint32_t phraseLength) {
	if (phraseLength == 0) {
		return 0;
	}
	uint32_t local = position % phraseLength;
	return static_cast<uint8_t>(std::min<uint32_t>(
	    kCellBorderPadCount - 1, (static_cast<uint64_t>(local) * kCellBorderPadCount) / phraseLength));
}

Geometry::ProgressFill Geometry::progressFill(uint64_t positionQ16, uint32_t phraseLength) {
	if (phraseLength == 0) {
		return {};
	}
	uint64_t phraseQ16 = static_cast<uint64_t>(phraseLength) << 16;
	uint64_t localQ16 = positionQ16 % phraseQ16;
	uint64_t padPositionQ16 = localQ16 * kCellBorderPadCount / phraseLength;
	return {.fullPads = static_cast<uint8_t>(std::min<uint64_t>(padPositionQ16 >> 16, kCellBorderPadCount)),
	        .leadingBrightness = static_cast<uint16_t>(padPositionQ16 & 0xFFFF)};
}

Geometry::ProgressFill Geometry::linearProgressFill(uint32_t elapsed, uint32_t duration) {
	if (duration == 0) {
		return {};
	}
	uint64_t clampedElapsed = std::min(elapsed, duration);
	uint64_t padPositionQ16 = clampedElapsed * kCellBorderPadCount * 65536ULL / duration;
	return {.fullPads = static_cast<uint8_t>(std::min<uint64_t>(padPositionQ16 >> 16, kCellBorderPadCount)),
	        .leadingBrightness = static_cast<uint16_t>(padPositionQ16 & 0xFFFF)};
}

uint16_t Geometry::beatPulseBrightness(uint64_t positionQ16, uint32_t beatLength) {
	if (beatLength == 0) {
		return 0;
	}
	uint64_t beatQ16 = static_cast<uint64_t>(beatLength) << 16;
	uint64_t phase = positionQ16 % beatQ16;
	uint64_t half = beatQ16 / 2;
	uint64_t distanceFromDimPoint = phase <= half ? half - phase : phase - half;
	return static_cast<uint16_t>(std::min<uint64_t>(65535, distanceFromDimPoint * 65535 / std::max<uint64_t>(1, half)));
}

void FanOutRouter::clear() {
	used_.fill(false);
	slotOutputIds_.fill(0);
}

bool FanOutRouter::sourceFeedsItself(SlotId destination, SourceKey source) const {
	return source.type == SourceType::SONG_TRACK && source.songTrackId != 0
	       && source.songTrackId == slotOutputIds_[indexOf(destination)];
}

void FanOutRouter::bindSlotOutput(SlotId slot, uint32_t outputId) {
	slotOutputIds_[indexOf(slot)] = outputId;
}

bool FanOutRouter::subscribe(TapSubscription subscription) {
	if (sourceFeedsItself(subscription.slot, subscription.source)
	    || (subscription.source.type == SourceType::SONG_TRACK && !subscription.sourceIsAudio)) {
		return false;
	}
	auto index = indexOf(subscription.slot);
	subscriptions_[index] = subscription;
	used_[index] = true;
	return true;
}

bool FanOutRouter::includeOutputInLoopStationMix(uint32_t outputId) const {
	if (outputId == 0)
		return true;
	return std::find(slotOutputIds_.begin(), slotOutputIds_.end(), outputId) == slotOutputIds_.end();
}

size_t FanOutRouter::sourceRenderCount() const {
	size_t count = 0;
	for (size_t i = 0; i < subscriptions_.size(); ++i) {
		if (!used_[i]) {
			continue;
		}
		bool first = true;
		for (size_t j = 0; j < i; ++j) {
			if (used_[j] && subscriptions_[j].source == subscriptions_[i].source) {
				first = false;
				break;
			}
		}
		count += first;
	}
	return count;
}

size_t FanOutRouter::monitorReturnCount() const {
	size_t count = 0;
	for (size_t i = 0; i < subscriptions_.size(); ++i) {
		auto const& current = subscriptions_[i];
		if (!used_[i] || !current.monitoring || !isPhysicalSource(current.source.type)) {
			continue;
		}
		bool first = true;
		for (size_t j = 0; j < i; ++j) {
			auto const& prior = subscriptions_[j];
			if (used_[j] && prior.monitoring && prior.source == current.source
			    && prior.inputFxFingerprint == current.inputFxFingerprint) {
				first = false;
				break;
			}
		}
		count += first;
	}
	return count;
}

uint16_t RecorderBudget::weight(RecorderCost cost) {
	return std::clamp<uint16_t>(cost.channels, 1, 2);
}

bool RecorderBudget::reserve(SlotId slot, RecorderCost cost) {
	auto index = indexOf(slot);
	uint16_t requested = weight(cost);
	uint16_t withoutCurrent = used_ - reservations_[index];
	if (withoutCurrent + requested > kCapacity) {
		return false;
	}
	used_ = withoutCurrent + requested;
	reservations_[index] = requested;
	return true;
}

void RecorderBudget::release(SlotId slot) {
	auto index = indexOf(slot);
	used_ -= reservations_[index];
	reservations_[index] = 0;
}

} // namespace deluge::loop_station
