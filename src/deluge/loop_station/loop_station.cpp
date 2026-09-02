/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "loop_station/loop_station.h"

#include <algorithm>

namespace deluge::loop_station {
namespace {
constexpr uint32_t kDoubleTapMs = 350;

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
	settings.startMode =
	    clampEnum<StartMode, StartMode::FADE>(static_cast<uint32_t>(settings.startMode), StartMode::IMMEDIATE);
	settings.stopMode =
	    clampEnum<StopMode, StopMode::FADE>(static_cast<uint32_t>(settings.stopMode), StopMode::LOOP_END);
	settings.fadeTimeBars = std::max<uint16_t>(settings.fadeTimeBars, 1);
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
	global.quantize =
	    clampEnum<QuantizeMode, QuantizeMode::MEASURE>(static_cast<uint32_t>(global.quantize), QuantizeMode::SMART);
	global.startFromStopped = clampEnum<StartFromStopped, StartFromStopped::FROM_START>(
	    static_cast<uint32_t>(global.startFromStopped), StartFromStopped::RESUME_QUANTIZED);
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
	switch (parameter) {
	case GlobalParameter::RECORD_ACTION:
		return static_cast<int32_t>(model.global.recordAction);
	case GlobalParameter::QUANTIZE:
		return static_cast<int32_t>(model.global.quantize);
	case GlobalParameter::START_FROM_STOPPED:
		return static_cast<int32_t>(model.global.startFromStopped);
	case GlobalParameter::DIM_BORDER_COLOUR:
		return static_cast<int32_t>(model.global.dimBorderColour);
	}
	return 0;
}

void writeGlobalParameter(SongModel& model, GlobalParameter parameter, int32_t value) {
	switch (parameter) {
	case GlobalParameter::RECORD_ACTION:
		model.global.recordAction = clampEnum<RecordAction, RecordAction::RECORD_PLAY_OVERDUB>(
		    static_cast<uint32_t>(value), RecordAction::RECORD_PLAY_OVERDUB);
		break;
	case GlobalParameter::QUANTIZE:
		model.global.quantize =
		    clampEnum<QuantizeMode, QuantizeMode::MEASURE>(static_cast<uint32_t>(value), QuantizeMode::SMART);
		break;
	case GlobalParameter::START_FROM_STOPPED:
		model.global.startFromStopped = clampEnum<StartFromStopped, StartFromStopped::FROM_START>(
		    static_cast<uint32_t>(value), StartFromStopped::RESUME_QUANTIZED);
		break;
	case GlobalParameter::DIM_BORDER_COLOUR:
		model.global.dimBorderColour =
		    clampEnum<PadColour, PadColour::WHITE>(static_cast<uint32_t>(value), PadColour::BLUE);
		break;
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
	case TrackParameter::LOOP_SYNC:
		return settings.loopSync;
	case TrackParameter::START_MODE:
		return static_cast<int32_t>(settings.startMode);
	case TrackParameter::STOP_MODE:
		return static_cast<int32_t>(settings.stopMode);
	case TrackParameter::FADE_TIME_BARS:
		return settings.fadeTimeBars;
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
	case TrackParameter::LOOP_SYNC:
		settings.loopSync = value != 0;
		break;
	case TrackParameter::START_MODE:
		settings.startMode = clampEnum<StartMode, StartMode::FADE>(static_cast<uint32_t>(value), StartMode::IMMEDIATE);
		break;
	case TrackParameter::STOP_MODE:
		settings.stopMode = clampEnum<StopMode, StopMode::FADE>(static_cast<uint32_t>(value), StopMode::LOOP_END);
		break;
	case TrackParameter::FADE_TIME_BARS:
		settings.fadeTimeBars = static_cast<uint16_t>(std::clamp<int32_t>(value, 1, UINT16_MAX));
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

uint32_t Engine::quantizedStart(QuantizeMode mode, uint32_t tick, uint32_t barLength) {
	if (mode != QuantizeMode::MEASURE || barLength == 0) {
		return tick;
	}
	uint32_t remainder = tick % barLength;
	return remainder == 0 ? tick : tick + (barLength - remainder);
}

uint32_t Engine::smartStop(uint32_t tick, uint32_t barLength, uint32_t eighthNoteLength) {
	if (barLength == 0) {
		return tick;
	}
	uint32_t lower = tick - tick % barLength;
	uint32_t upper = lower + barLength;
	uint32_t distanceLower = tick - lower;
	uint32_t distanceUpper = upper - tick;
	if (distanceLower <= eighthNoteLength) {
		return lower;
	}
	if (distanceUpper <= eighthNoteLength) {
		return upper;
	}
	return tick;
}

bool Engine::isDoubleTap(TrackRuntime& runtime, uint32_t gestureTimeMs) {
	bool result = runtime.lastTapTimeMs != std::numeric_limits<uint32_t>::max()
	              && gestureTimeMs - runtime.lastTapTimeMs <= kDoubleTapMs;
	runtime.lastTapTimeMs = gestureTimeMs;
	return result;
}

void Engine::centrePressed(SlotId slot, Transport const& transport, uint32_t gestureTimeMs) {
	auto& runtime = track(slot);
	if (runtime.state == TrackState::ERROR) {
		runtime.error = Error::NONE;
		transition(slot, runtime.stateBeforeCommit);
		return;
	}
	if (runtime.state == TrackState::COMMITTING) {
		return;
	}
	bool doubleTap = isDoubleTap(runtime, gestureTimeMs);
	if (doubleTap && runtime.state == TrackState::OVERDUBBING) {
		if (delegate_) {
			delegate_->abortCapture(slot, true);
		}
		runtime.event = {};
		transition(slot, TrackState::PLAYING);
		stop(slot, transport.tick);
		return;
	}

	auto const& settings = model_.tracks[indexOf(slot)].settings;
	switch (runtime.state) {
	case TrackState::EMPTY: {
		uint32_t target = quantizedStart(model_.global.quantize, transport.tick, transport.barLength);
		if (target == transport.tick) {
			execute(slot, PendingAction::START_RECORDING, transport, transport.tick);
		}
		else {
			schedule(slot, PendingAction::START_RECORDING, target, transport.tick);
		}
		break;
	}
	case TrackState::RECORDING: {
		uint32_t target = transport.tick;
		if (model_.global.quantize == QuantizeMode::MEASURE) {
			target = quantizedStart(QuantizeMode::MEASURE, transport.tick + 1, transport.barLength);
		}
		else if (model_.global.quantize == QuantizeMode::SMART) {
			target = smartStop(transport.tick, transport.barLength, transport.eighthNoteLength);
		}
		if (target > transport.tick) {
			schedule(slot, PendingAction::FINISH_RECORDING, target, target);
		}
		else {
			finishCapture(slot, false, transport.tick, target);
		}
		break;
	}
	case TrackState::PLAYING:
		if (doubleTap) {
			stop(slot, transport.tick);
		}
		else if (settings.playback == PlaybackMode::ONE_SHOT) {
			runtime.playbackStartTick = transport.tick;
		}
		else if (runtime.finishingOverdub) {
			// Previous overdub still finishing in background - ignore new overdub request
		}
		else {
			// Overdub starts immediately at current position.
			execute(slot, PendingAction::START_OVERDUB, transport, transport.tick);
		}
		break;
	case TrackState::OVERDUBBING:
		if (doubleTap) {
			if (delegate_) {
				delegate_->abortCapture(slot, true);
			}
			stop(slot, transport.tick);
		}
		else {
			// Press center to stop overdub early - return to PLAY immediately,
			// but recorder continues in background for the full phrase.
			if (!runtime.finishingOverdub) {
				runtime.finishingOverdub = true;
				transition(slot, TrackState::PLAYING);
			}
		}
		break;
	case TrackState::STOPPED:
		if (settings.playback == PlaybackMode::ONE_SHOT
		    || model_.global.startFromStopped == StartFromStopped::FROM_START) {
			beginPlayback(slot, transport, true);
		}
		else if (!settings.loopSync) {
			runtime.playbackStartTick = transport.tick - runtime.savedStopPhase;
			beginPlayback(slot, transport, false);
		}
		else {
			uint32_t target = quantizedStart(QuantizeMode::MEASURE, transport.tick, transport.barLength);
			if (target == transport.tick)
				beginPlayback(slot, transport, false);
			else
				schedule(slot, PendingAction::RESUME, target, target);
		}
		break;
	case TrackState::COMMITTING:
	case TrackState::ERROR:
		break;
	}
}

void Engine::schedule(SlotId slot, PendingAction action, uint32_t atTick, uint32_t requestedTick) {
	track(slot).event = {.action = action, .tick = atTick, .requestedTick = requestedTick};
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
		stop(slot, transport.tick, true);
		break;
	case PendingAction::RETRIGGER:
		beginPlayback(slot, transport, true);
		break;
	case PendingAction::RESUME:
		beginPlayback(slot, transport, false);
		break;
	case PendingAction::NONE:
		break;
	}
}

void Engine::startAll(Transport const& transport) {
	uint32_t target = quantizedStart(model_.global.quantize, transport.tick, transport.barLength);
	for (size_t i = 0; i < runtime_.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& runtime = runtime_[i];
		if (runtime.state != TrackState::STOPPED || runtime.event.pending()) {
			continue;
		}

		auto const& settings = model_.tracks[i].settings;
		bool fromStart = settings.playback == PlaybackMode::ONE_SHOT
		                 || model_.global.startFromStopped == StartFromStopped::FROM_START;
		if (!fromStart && !settings.loopSync) {
			runtime.playbackStartTick = target - runtime.savedStopPhase;
		}
		auto action = fromStart ? PendingAction::RETRIGGER : PendingAction::RESUME;
		if (target == transport.tick) {
			execute(slot, action, transport, target);
		}
		else {
			schedule(slot, action, target, target);
		}
	}
}

void Engine::stopAll(Transport const& transport) {
	uint32_t target = transport.tick;
	if (model_.global.quantize == QuantizeMode::MEASURE) {
		target = quantizedStart(QuantizeMode::MEASURE, transport.tick + 1, transport.barLength);
	}
	for (size_t i = 0; i < runtime_.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& runtime = runtime_[i];
		// A cumulative overdub must finish its complete backing pass. Group Stop
		// affects stable playback only and never truncates an active capture.
		if (runtime.state != TrackState::PLAYING || runtime.finishingOverdub || runtime.event.pending()) {
			continue;
		}
		if (target == transport.tick) {
			stop(slot, target, true);
		}
		else {
			schedule(slot, PendingAction::STOP, target, target);
		}
	}
}

bool Engine::canStartAll() const {
	return std::any_of(runtime_.begin(), runtime_.end(), [](TrackRuntime const& runtime) {
		return runtime.state == TrackState::STOPPED && !runtime.event.pending();
	});
}

bool Engine::canStopAll() const {
	return std::any_of(runtime_.begin(), runtime_.end(), [](TrackRuntime const& runtime) {
		return runtime.state == TrackState::PLAYING && !runtime.finishingOverdub && !runtime.event.pending();
	});
}

void Engine::beginCapture(SlotId slot, bool overdub, uint32_t tick, Transport const&) {
	auto& runtime = track(slot);
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
	runtime.event = {};
	runtime.overdubStartTick = 0;
	runtime.overdubEndTick = 0;
	runtime.finishingOverdub = false;
	runtime.error = Error::NONE;
	if (overdub) {
		runtime.overdubStartTick = tick;
		// Overdub recorder runs for exactly one full phrase length.
		setOverdubEnd(runtime, tick);
	}
	transition(slot, overdub ? TrackState::OVERDUBBING : TrackState::RECORDING);
}

void Engine::finishCapture(SlotId slot, bool overdub, uint32_t tick, uint32_t requestedTick) {
	auto& runtime = track(slot);
	runtime.overdubEndTick = 0;
	uint32_t endTick = requestedTick;
	if (endTick <= runtime.captureStartTick) {
		endTick = tick;
	}
	uint32_t length = endTick - runtime.captureStartTick;
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
	// Overdubs always capture exactly one full phrase length - never crop.
	// Crop only applies to initial recording with Smart quantization.
	bool crop = !overdub && requestedTick < tick;
	auto finish = delegate_ ? delegate_->finishCapture(slot, overdub, length, crop) : CaptureFinish{};
	if (!finish.accepted) {
		fail(slot, finish.error == Error::NONE ? Error::SD_CARD : finish.error);
		return;
	}
	uint32_t committedLength = finish.committedLength ? finish.committedLength : length;
	runtime.event = {};
	if (finish.asynchronous) {
		runtime.committingLength = committedLength;
		runtime.committingOverdub = overdub;
		runtime.stateBeforeCommit =
		    overdub ? TrackState::PLAYING
		            : (model_.global.recordAction == RecordAction::RECORD_OVERDUB_PLAY ? TrackState::OVERDUBBING
		                                                                               : TrackState::PLAYING);
		transition(slot, TrackState::COMMITTING);
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
	TrackState destination = TrackState::PLAYING;
	if (!exiting_ && !overdub && model_.global.recordAction == RecordAction::RECORD_OVERDUB_PLAY
	    && model_.tracks[indexOf(slot)].settings.playback == PlaybackMode::LOOP) {
		// REC→DUB starts one overdub pass at the fresh phrase boundary.
		destination = TrackState::OVERDUBBING;
		// An asynchronous initial take may finish after its original boundary.
		// Start the dependent overdub at the completion position so it always gets
		// a full phrase instead of trying to catch up a partially elapsed pass.
		runtime.captureStartTick = completionTick ? completionTick : runtime.captureStartTick + length;
		if (delegate_
		    && (!delegate_->preflight(slot, true) || !delegate_->beginCapture(slot, true, runtime.captureStartTick))) {
			destination = TrackState::PLAYING;
		}
		else {
			setOverdubEnd(runtime, runtime.captureStartTick);
		}
	}
	transition(slot, destination);
}

void Engine::setOverdubEnd(TrackRuntime& runtime, uint32_t tick) {
	runtime.overdubEndTick = 0;
	if (runtime.phraseLength == 0) {
		return;
	}
	uint64_t end = static_cast<uint64_t>(tick) + runtime.phraseLength;
	runtime.overdubEndTick = static_cast<uint32_t>(std::min<uint64_t>(end, std::numeric_limits<uint32_t>::max()));
}

void Engine::stop(SlotId slot, uint32_t tick, bool forceImmediate) {
	auto& runtime = track(slot);
	auto const& settings = model_.tracks[indexOf(slot)].settings;
	if (!forceImmediate && runtime.state == TrackState::PLAYING && runtime.phraseLength != 0) {
		if (settings.stopMode == StopMode::LOOP_END) {
			uint32_t phase = loopPhaseAt(tick, runtime.playbackStartTick, runtime.phraseLength);
			uint32_t remaining = phase == 0 ? runtime.phraseLength : runtime.phraseLength - phase;
			schedule(slot, PendingAction::STOP, tick + remaining, tick + remaining);
			return;
		}
		if (settings.stopMode == StopMode::FADE) {
			runtime.fadeStartTick = tick;
			runtime.fadeEndTick = tick + settings.fadeTimeBars;
			// processTick expands the bar-relative endpoint using the live bar length.
			runtime.fadeStartGain = runtime.playbackGain;
			runtime.fadeEndGain = 0;
			runtime.event = {.action = PendingAction::STOP, .tick = 0, .requestedTick = tick};
			return;
		}
	}
	runtime.savedStopPhase = loopPhaseAt(tick, runtime.playbackStartTick, runtime.phraseLength);
	if (!settings.loopSync && delegate_) {
		runtime.savedStopPhase = delegate_->playbackPhase(slot, runtime.savedStopPhase);
	}
	runtime.event = {};
	runtime.overdubEndTick = 0;
	if (runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING) {
		finishCapture(slot, runtime.state == TrackState::OVERDUBBING, tick, tick);
		if (runtime.state == TrackState::COMMITTING) {
			runtime.stateBeforeCommit = TrackState::STOPPED;
			return;
		}
	}
	if (runtime.currentRevision == 0) {
		transition(slot, TrackState::EMPTY);
	}
	else {
		setPlaybackGain(slot, 0, true);
		transition(slot, TrackState::STOPPED);
	}
}

void Engine::beginPlayback(SlotId slot, Transport const& transport, bool fromStart) {
	auto& runtime = track(slot);
	auto const& settings = model_.tracks[indexOf(slot)].settings;
	if (fromStart)
		runtime.playbackStartTick = transport.tick;
	if (settings.startMode == StartMode::FADE) {
		setPlaybackGain(slot, 0, true);
		runtime.fadeStartGain = 0;
		runtime.fadeEndGain = 1 << 24;
		runtime.fadeStartTick = transport.tick;
		runtime.fadeEndTick = transport.tick + settings.fadeTimeBars * std::max<uint32_t>(1, transport.barLength);
	}
	else {
		setPlaybackGain(slot, 1 << 24, true);
		runtime.fadeStartTick = runtime.fadeEndTick = 0;
	}
	transition(slot, TrackState::PLAYING);
}

void Engine::setPlaybackGain(SlotId slot, int32_t gain, bool forceNotify) {
	auto& runtime = track(slot);
	bool changed = gain != runtime.playbackGain;
	runtime.playbackGain = gain;
	if (delegate_ && (changed || forceNotify)) {
		delegate_->playbackGainChanged(slot, gain);
	}
}

void Engine::updateFade(SlotId slot, uint32_t tick) {
	auto& runtime = track(slot);
	if (runtime.fadeEndTick <= runtime.fadeStartTick)
		return;
	int32_t gain;
	if (tick >= runtime.fadeEndTick)
		gain = runtime.fadeEndGain;
	else if (tick <= runtime.fadeStartTick)
		gain = runtime.fadeStartGain;
	else {
		int64_t elapsed = tick - runtime.fadeStartTick;
		int64_t duration = runtime.fadeEndTick - runtime.fadeStartTick;
		gain = runtime.fadeStartGain
		       + static_cast<int32_t>((static_cast<int64_t>(runtime.fadeEndGain - runtime.fadeStartGain) * elapsed)
		                              / duration);
	}
	if (gain != runtime.playbackGain) {
		setPlaybackGain(slot, gain);
	}
	if (tick >= runtime.fadeEndTick)
		runtime.fadeStartTick = runtime.fadeEndTick = 0;
}

void Engine::processTick(Transport const& transport) {
	for (size_t i = 0; i < runtime_.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& runtime = runtime_[i];
		if (runtime.event.action == PendingAction::STOP && runtime.event.tick == 0) {
			auto bars = std::max<uint32_t>(1, model_.tracks[i].settings.fadeTimeBars);
			runtime.fadeEndTick = runtime.event.requestedTick + bars * std::max<uint32_t>(1, transport.barLength);
			runtime.event.tick = runtime.fadeEndTick;
		}
		updateFade(slot, transport.tick);
		if (runtime.event.pending() && transport.tick >= runtime.event.tick) {
			auto event = runtime.event;
			runtime.event = {};
			execute(slot, event.action, transport, event.requestedTick);
		}

		// An overdub records for exactly one full phrase length from its start tick,
		// then auto-commits and returns to PLAYING.
		if ((runtime.state == TrackState::OVERDUBBING || runtime.finishingOverdub) && runtime.overdubEndTick != 0
		    && transport.tick >= runtime.overdubEndTick) {
			uint32_t endTick = runtime.overdubEndTick;
			finishCapture(slot, true, endTick, endTick);
		}

		auto const& settings = model_.tracks[i].settings;
		if (runtime.state == TrackState::RECORDING && settings.durationBars != kManualDuration
		    && !runtime.event.pending() && transport.barLength != 0) {
			uint64_t target = static_cast<uint64_t>(runtime.captureStartTick)
			                  + static_cast<uint64_t>(settings.durationBars) * transport.barLength;
			target = std::min<uint64_t>(target, kMaxLoopSequenceLength);
			if (transport.tick >= target) {
				finishCapture(slot, false, static_cast<uint32_t>(target), static_cast<uint32_t>(target));
			}
			else {
				schedule(slot, PendingAction::FINISH_RECORDING, static_cast<uint32_t>(target),
				         static_cast<uint32_t>(target));
			}
		}

		if (runtime.state == TrackState::PLAYING && settings.playback == PlaybackMode::ONE_SHOT
		    && runtime.phraseLength != 0 && transport.tick - runtime.playbackStartTick >= runtime.phraseLength) {
			transition(slot, TrackState::STOPPED);
		}
	}
}

bool Engine::requestExit(Transport const& transport) {
	exiting_ = true;
	for (size_t i = 0; i < runtime_.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& runtime = runtime_[i];
		if (runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING
		    || runtime.finishingOverdub) {
			uint32_t target = transport.tick;
			if ((runtime.state == TrackState::OVERDUBBING || runtime.finishingOverdub)
			    && runtime.overdubEndTick > transport.tick) {
				// The output-bounce overdub must commit a complete, aligned pass.
				target = runtime.overdubEndTick;
			}
			else if (model_.global.quantize == QuantizeMode::MEASURE) {
				target = quantizedStart(QuantizeMode::MEASURE, transport.tick + 1, transport.barLength);
			}
			else if (model_.global.quantize == QuantizeMode::SMART) {
				target = smartStop(transport.tick, transport.barLength, transport.eighthNoteLength);
			}
			auto action = runtime.state == TrackState::RECORDING ? PendingAction::FINISH_RECORDING
			                                                     : PendingAction::FINISH_OVERDUB;
			if (target > transport.tick) {
				schedule(slot, action, target, target);
			}
			else {
				finishCapture(slot, runtime.state == TrackState::OVERDUBBING || runtime.finishingOverdub,
				              transport.tick, target);
			}
		}
		else {
			// Entry/retrigger intentions are not captures and must not keep the
			// view alive after the user asks to leave.
			runtime.event = {};
		}
	}
	return exitReady();
}

bool Engine::exitReady() const {
	if (!exiting_) {
		return false;
	}
	for (auto const& runtime : runtime_) {
		if (runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING
		    || runtime.finishingOverdub || runtime.state == TrackState::COMMITTING || runtime.event.pending()) {
			return false;
		}
	}
	return true;
}

bool Engine::anyCapturing() const {
	return std::any_of(runtime_.begin(), runtime_.end(), [](TrackRuntime const& runtime) {
		return runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING
		       || runtime.finishingOverdub || runtime.state == TrackState::COMMITTING;
	});
}

uint32_t Engine::ticksUntilNextEvent(uint32_t currentTick) const {
	uint32_t result = std::numeric_limits<uint32_t>::max();
	for (auto const& runtime : runtime_) {
		if (runtime.event.pending()) {
			result = std::min(result, runtime.event.tick > currentTick ? runtime.event.tick - currentTick : 0);
		}
		if ((runtime.state == TrackState::OVERDUBBING || runtime.finishingOverdub) && runtime.overdubEndTick != 0) {
			result = std::min(result, runtime.overdubEndTick > currentTick ? runtime.overdubEndTick - currentTick : 0);
		}
	}
	return result;
}

void Engine::clearHeld(SlotId slot) {
	auto& runtime = track(slot);
	if (runtime.state != TrackState::STOPPED || runtime.state == TrackState::COMMITTING) {
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
	if (runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING
	    || runtime.state == TrackState::COMMITTING || (redo ? !runtime.redoAvailable : !runtime.undoAvailable)) {
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
	if (runtime.state != TrackState::COMMITTING) {
		return;
	}
	if (!success) {
		fail(slot, error == Error::NONE ? Error::SD_CARD : error);
		return;
	}
	bool overdub = runtime.committingOverdub;
	uint32_t length = runtime.committingLength;
	completeCapture(slot, overdub, length, completionTick);
	if (runtime.stateBeforeCommit == TrackState::STOPPED) {
		transition(slot, TrackState::STOPPED);
	}
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
	bool hadPhrase = runtime.currentRevision != 0;
	if (runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING
	    || runtime.finishingOverdub) {
		if (delegate_) {
			delegate_->abortCapture(slot, runtime.state == TrackState::OVERDUBBING || runtime.finishingOverdub);
		}
	}
	runtime.event = {};
	runtime.overdubEndTick = 0;
	runtime.finishingOverdub = false;
	runtime.error = error;
	transition(slot, TrackState::ERROR);
	// Error is a visual latch; a subsequent render/ack can restore the playable state.
	runtime.stateBeforeCommit = hadPhrase ? TrackState::PLAYING : TrackState::EMPTY;
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
