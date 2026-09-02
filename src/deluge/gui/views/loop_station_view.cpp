/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui/views/loop_station_view.h"

#include "gui/colour/palette.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/loop_station/settings.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "gui/views/session_view.h"
#include "gui/views/view.h"
#include "hid/button.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/led/indicator_leds.h"
#include "model/clip/audio_clip.h"
#include "model/model_stack.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "model/song/song.h"
#include "model/voice/voice_sample.h"
#include "playback/mode/arrangement.h"
#include "playback/mode/playback_mode.h"
#include "playback/mode/session.h"
#include "playback/playback_handler.h"
#include "processing/audio_output.h"
#include "processing/engines/audio_engine.h"
#include "util/misc.h"

#include <algorithm>
#include <limits>

using namespace deluge;
using namespace deluge::loop_station;
using deluge::gui::colours::black;

namespace {
constexpr uint32_t kDeleteHoldDurationSamples = 2 * kSampleRate;
}

PLACE_SDRAM_BSS LoopStationView loopStationView{};

bool LoopStationView::opened() {
	recordingModeBefore_ = playbackHandler.recording;
	recorderBudget_ = {};
	engine_.emplace(currentSong->loopStation, static_cast<EngineDelegate*>(this));
	exitRequested_ = false;
	monitoringActive_ = true;
	selectedSlot_.reset();
	monitoringCaptureMask_ = 0;
	centreHeldMask_.fill(0);
	borderHeldMask_.fill(0);
	centreGestureActive_.fill(false);
	centreGestureDispatched_.fill(false);
	restoreBoundPhrases();
	refreshMonitoring();
	focusRegained();
	return true;
}

void LoopStationView::focusRegained() {
	clipMenuContext_ = false;
	indicator_leds::setLedState(IndicatorLED::CROSS_SCREEN_EDIT, true);
	updateModControllableTarget();
	updateUndoLed();
	uiNeedsRendering(this, 0xFFFFFFFF, 0xFFFFFFFF);
}

Transport LoopStationView::transport() const {
	// lastSwungTickActioned is the most recent scheduler event, not necessarily
	// the musical position at the pad gesture. Using it as the record endpoint
	// can assign a much shorter or longer clip length and make AudioClip
	// time-stretch otherwise correctly recorded audio.
	uint32_t tick = playbackHandler.playbackState ? static_cast<uint32_t>(musicalPositionQ16() >> 16) : 0;
	uint32_t bar = std::max<uint32_t>(1, currentSong->getBarLength());
	return {.tick = tick,
	        .barLength = bar,
	        .eighthNoteLength = std::max<uint32_t>(1, currentSong->getQuarterNoteLength() / 2),
	        .playing = playbackHandler.playbackState != 0,
	        .arrangement = playbackHandler.playbackState && currentPlaybackMode == &arrangement};
}

uint64_t LoopStationView::musicalPositionQ16() const {
	if (!playbackHandler.playbackState) {
		return 0;
	}
	if (!playbackHandler.isInternalClockActive()) {
		float position = std::max(0.0f, playbackHandler.getCurrentInternalTickFloatFollowingExternalClock());
		return static_cast<uint64_t>(position * 65536.0f);
	}
	uint32_t remainder = 0;
	int64_t tick = std::max<int64_t>(0, playbackHandler.getCurrentInternalTickCount(&remainder));
	uint32_t samplesPerTick = std::max<uint32_t>(1, currentSong->getTimePerTimerTickRounded());
	uint32_t fraction =
	    static_cast<uint32_t>(std::min<uint64_t>(65535, static_cast<uint64_t>(remainder) * 65536 / samplesPerTick));
	return (static_cast<uint64_t>(tick) << 16) | fraction;
}

uint8_t LoopStationView::capturingMask() const {
	uint8_t result = 0;
	if (!engine_) {
		return result;
	}
	for (size_t i = 0; i < kTrackCount; ++i) {
		auto state = engine_->track(static_cast<SlotId>(i)).state;
		if (state == TrackState::RECORDING || state == TrackState::OVERDUBBING) {
			result |= 1u << i;
		}
	}
	return result;
}

void LoopStationView::updateRecordingMode() {
	playbackHandler.recording = engine_ && engine_->anyCapturing() ? RecordingMode::NORMAL : recordingModeBefore_;
}

void LoopStationView::updateUndoLed() {
	bool available = false;
	if (engine_ && selectedSlot_) {
		auto const& runtime = engine_->track(*selectedSlot_);
		bool busy = runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING
		            || runtime.finishingOverdub || runtime.state == TrackState::COMMITTING;
		available = !busy && (runtime.undoAvailable || runtime.redoAvailable);
	}
	indicator_leds::setLedState(IndicatorLED::BACK, available);
}

void LoopStationView::updateModControllableTarget() {
	if (selectedSlot_) {
		AudioClip* clip = backingClip(*selectedSlot_);
		if (clip) {
			currentSong->setCurrentClip(clip);
			view.setActiveModControllableTimelineCounter(clip);
			return;
		}
	}
	view.setActiveModControllableTimelineCounter(currentSong);
}

void LoopStationView::restoreBoundPhrases() {
	for (int32_t i = 0; i < currentSong->sessionClips.getNumElements(); ++i) {
		Clip* clip = currentSong->sessionClips.getClipAtIndex(i);
		if (clip->type != ClipType::AUDIO) {
			continue;
		}
		auto* audioClip = static_cast<AudioClip*>(clip);
		if (audioClip->loopStationSlotId >= 1 && audioClip->loopStationSlotId <= kTrackCount) {
			auto slot = static_cast<SlotId>(audioClip->loopStationSlotId - 1);
			audioClip->loopStationTempoSync =
			    currentSong->loopStation.tracks[static_cast<size_t>(slot)].settings.loopSync;
			auto& history = currentSong->loopStationRevisionHistory[static_cast<size_t>(slot)];
			history.seedFromClip(*audioClip);
			if (audioClip->isEmpty(false) && history.revisions.empty()) {
				continue;
			}
			uint32_t phraseLength = audioClip->loopLength;
			if (!history.hasCurrent() && !history.revisions.empty()) {
				phraseLength = history.revisions.front().loopLength;
			}
			bool hasPhrase = !audioClip->isEmpty(false) && history.hasCurrent();
			engine_->restoreBoundPhrase(slot, phraseLength, hasPhrase && currentSong->isClipActive(audioClip),
			                            hasPhrase, history.revisions.size(), history.cursor);
		}
	}
}

AudioClip* LoopStationView::backingClip(SlotId slot) const {
	uint8_t wanted = static_cast<uint8_t>(slot) + 1;
	for (int32_t i = 0; i < currentSong->sessionClips.getNumElements(); ++i) {
		Clip* clip = currentSong->sessionClips.getClipAtIndex(i);
		if (clip->type == ClipType::AUDIO && static_cast<AudioClip*>(clip)->loopStationSlotId == wanted) {
			return static_cast<AudioClip*>(clip);
		}
	}
	return nullptr;
}

AudioClip* LoopStationView::ensureBackingClip(SlotId slot) {
	AudioClip* result = backingClip(slot);
	if (!result) {
		result = sessionView.createLoopStationClip(static_cast<uint8_t>(slot));
	}
	if (!result) {
		return nullptr;
	}
	auto* output = static_cast<AudioOutput*>(result->output);
	auto const& settings = currentSong->loopStation.tracks[static_cast<size_t>(slot)].settings;
	result->loopStationTempoSync = settings.loopSync;
	switch (settings.source) {
	case SourceType::LEFT:
		output->inputChannel = AudioInputChannel::LEFT;
		break;
	case SourceType::RIGHT:
		output->inputChannel = AudioInputChannel::RIGHT;
		break;
	case SourceType::STEREO:
		output->inputChannel = AudioInputChannel::STEREO;
		break;
	case SourceType::BALANCED:
		output->inputChannel = AudioInputChannel::BALANCED;
		break;
	case SourceType::MIX_PRE_FX:
		output->inputChannel = AudioInputChannel::MIX;
		break;
	case SourceType::MIX_POST_FX:
		output->inputChannel = AudioInputChannel::OUTPUT;
		break;
	case SourceType::SONG_TRACK: {
		Output* source = currentSong->getOutputFromIndex(settings.sourceTrackId);
		if (source && source != output && source->type == OutputType::AUDIO) {
			output->inputChannel = AudioInputChannel::SPECIFIC_OUTPUT;
			output->setOutputRecordingFrom(source);
		}
		else {
			output->inputChannel = AudioInputChannel::NONE;
		}
		break;
	}
	}
	// A slot may have been created lazily by the first centre gesture. Refresh
	// after assigning its input so monitoring is audible for that very first take.
	refreshMonitoring();
	return result;
}

bool LoopStationView::preflight(SlotId slot, bool overdub) {
	AudioClip* clip = ensureBackingClip(slot);
	if (!clip || clip->recorder || overdub == clip->isEmpty(false)) {
		return false;
	}
	auto* output = static_cast<AudioOutput*>(clip->output);
	bool sourceValid =
	    output->inputChannel > AudioInputChannel::NONE
	    && (output->inputChannel != AudioInputChannel::SPECIFIC_OUTPUT || output->getOutputRecordingFrom());
	if (!sourceValid)
		return false;
	auto source = currentSong->loopStation.tracks[static_cast<size_t>(slot)].settings.source;
	uint8_t channels = source == SourceType::STEREO ? 2 : 1;
	return recorderBudget_.reserve(slot, {.channels = channels});
}

bool LoopStationView::beginCapture(SlotId slot, bool overdub, uint32_t tick) {
	AudioClip* clip = backingClip(slot);
	if (!clip || clip->recorder) {
		recorderBudget_.release(slot);
		return false;
	}

	size_t index = static_cast<size_t>(slot);
	// Reserve the append before creating the recorder, and retain the active
	// revision before an overdub. Allocation or SD failure can therefore never
	// strand the clip after its old Sample has been released.
	if (!currentSong->loopStationRevisionHistory[index].prepareAppend(*clip, overdub)) {
		recorderBudget_.release(slot);
		return false;
	}
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	auto* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	clip->activeIfNoSolo = true;
	clip->output->setActiveClip(modelStack);

	if (overdub && !clip->isEmpty(false)) {
		// Ensure the clip voice is playing so the output recorder captures the loop.
		// Use lastSwungTickActioned (scheduler tick) as the base, since resumePlayback
		// uses it to compute sequenceSyncStartedAtTick. Using musicalPositionQ16()
		// would cause a phase mismatch.
		uint32_t currentTick = playbackHandler.lastSwungTickActioned;
		auto const& runtime = engine_->track(slot);
		uint32_t length = std::max<int32_t>(1, clip->loopLength);
		uint32_t logicalPhase = loopPhaseAt(currentTick, runtime.playbackStartTick, length);
		// Translate logical phase to physical sample position using the recorded sample-zero offset.
		clip->setPos(
		    modelStack,
		    static_cast<int32_t>(samplePhaseForLoopPhase(logicalPhase, clip->loopStationSampleZeroTicks, length)),
		    true);
		clip->resumePlayback(modelStack, true);
		// An output overdub without a backing voice would create an input-only
		// replacement. Reject it before allocating or attaching the recorder.
		if (!clip->voiceSample) {
			recorderBudget_.release(slot);
			return false;
		}
	}

	playbackHandler.recording = RecordingMode::NORMAL;
	::Error error = clip->beginLinearRecording(modelStack, 0);
	if (error != ::Error::NONE) {
		recorderBudget_.release(slot);
		return false;
	}
	// Every Loop Station WAV is a fixed-gain dry revision. Explicitly disable
	// normalization and file alteration for first takes as well as overdubs.
	clip->recorder->allowFileAlterationAfter = false;
	clip->recorder->allowNormalization = false;
	if (overdub && clip->recorder) {
		// Capture stored audio plus monitored input before track FX and without
		// normalization. The recorder captures the clip output (loop) + input.
		clip->recorder->captureOutputPreFX = true;
		clip->recorder->captureOutputInputActive = true;
	}
	refreshMonitoring();
	return true;
}

CaptureFinish LoopStationView::finishCapture(SlotId slot, bool overdub, uint32_t length, bool crop) {
	recorderBudget_.release(slot);
	AudioClip* clip = backingClip(slot);
	if (!clip || !clip->recorder) {
		return {.accepted = false,
		        .error = overdub ? deluge::loop_station::Error::BACKING_UNAVAILABLE
		                         : deluge::loop_station::Error::ALLOCATION};
	}
	SampleRecorder* recorder = clip->recorder;
	// Reject without publishing the provisional WAV, and preserve the exact
	// failure reason instead of presenting every capture failure as an SD error.
	auto reject = [clip](deluge::loop_station::Error error) {
		clip->abortRecording();
		return CaptureFinish{.accepted = false, .error = error};
	};
	if (recorder->hadCardError)
		return reject(deluge::loop_station::Error::SD_CARD);
	if (recorder->reachedMaxFileSize)
		return reject(deluge::loop_station::Error::MAX_FILE_SIZE);
	if (overdub && recorder->captureOutputBackingMissing) {
		return reject(deluge::loop_station::Error::BACKING_UNAVAILABLE);
	}
	if (recorder->status == RecorderStatus::ABORTED)
		return reject(deluge::loop_station::Error::ALLOCATION);
	if (!recorder->numSamplesCaptured)
		return reject(deluge::loop_station::Error::SOURCE_REMOVED);

	auto* oldAudioFile = clip->sampleHolder.audioFile;
	String trackName;
	trackName.set(&clip->name);
	uint32_t targetLength;
	if (overdub) {
		// The first accepted take permanently owns the phrase length. Never use
		// AudioClip::loopLength as the authority here: generic recording code or a
		// corrupt runtime transition must not make an overdub change playback speed.
		if (!engine_ || engine_->track(slot).phraseLength == 0) {
			return reject(deluge::loop_station::Error::BACKING_UNAVAILABLE);
		}
		targetLength = engine_->track(slot).phraseLength;
	}
	else {
		targetLength = std::max<uint32_t>(1, length);
	}
	int32_t oldLength = clip->loopLength;
	clip->loopLength = targetLength;
	clip->originalLength = targetLength;

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	auto* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	if (oldLength != clip->loopLength) {
		clip->lengthChanged(modelStack, oldLength, nullptr);
	}
	// A late Smart stop keeps the bar-snapped musical length and trims the
	// corresponding excess from the recorder's loop endpoint. Overdubs always
	// retain the established phrase length and are never tempo-trimmed again.
	int32_t trimSamples = 0;
	if (crop && !overdub && engine_) {
		auto const& runtime = engine_->track(slot);
		uint32_t currentTick = static_cast<uint32_t>(musicalPositionQ16() >> 16);
		uint32_t performedLength = currentTick >= runtime.captureStartTick ? currentTick - runtime.captureStartTick : 0;
		if (performedLength > length) {
			double samples = static_cast<double>(performedLength - length)
			                 * static_cast<double>(playbackHandler.getTimePerInternalTickFloat());
			trimSamples = static_cast<int32_t>(std::min<double>(samples, std::numeric_limits<int32_t>::max()));
		}
	}
	// Both initial recording and overdub use the normal finish path.
	// Overdub recorders are configured to capture pre-FX output + input.
	clip->finishLinearRecording(modelStack, nullptr, trimSamples);
	clip->name.set(&trackName);

	bool accepted = clip->sampleHolder.audioFile && (!overdub || clip->sampleHolder.audioFile != oldAudioFile);
	if (accepted) {
		// For overdubs starting mid-phrase, the new cumulative WAV's physical sample 0
		// corresponds to the overdub start position, not the original phrase start.
		// Track this so playback translates logical phrase position to physical sample position.
		if (overdub && engine_) {
			auto const& runtime = engine_->track(slot);
			clip->loopStationSampleZeroTicks =
			    loopPhaseAt(runtime.overdubStartTick, runtime.playbackStartTick, targetLength);
		}
		else {
			clip->loopStationSampleZeroTicks = 0;
		}

		// Apply Loop Station track transpose to the recorded clip
		auto const& settings = currentSong->loopStation.tracks[static_cast<size_t>(slot)].settings;
		if (settings.transpose != 0 || settings.transposeCents != 0) {
			clip->sampleHolder.transpose = settings.transpose;
			clip->sampleHolder.cents = settings.transposeCents;
			clip->sampleHolder.recalculateNeutralPhaseIncrement();
		}

		if (!overdub) {
			// AudioClip normally time-stretches its sample to loopLength. Derive a
			// playable whole-tick endpoint which never exceeds the actual take,
			// then trim only the unused tail. The waveform itself is consequently
			// replayed at its recorded speed.
			double timePerTick = playbackHandler.getTimePerInternalTickFloat();
			uint64_t availableSamples = clip->sampleHolder.getDurationInSamples(false);
			uint64_t wantedSamples = static_cast<uint64_t>(timePerTick * targetLength);
			if (wantedSamples > availableSamples && timePerTick > 0.0) {
				targetLength = static_cast<uint32_t>(std::max<double>(1.0, availableSamples / timePerTick));
				wantedSamples = static_cast<uint64_t>(timePerTick * targetLength);
				int32_t lengthBeforeAdjustment = clip->loopLength;
				clip->loopLength = targetLength;
				clip->originalLength = targetLength;
				if (lengthBeforeAdjustment != clip->loopLength) {
					clip->lengthChanged(modelStack, lengthBeforeAdjustment, nullptr);
				}
			}
			if (wantedSamples && wantedSamples <= availableSamples) {
				clip->sampleHolder.endPos = clip->sampleHolder.startPos + wantedSamples;
			}
		}
		size_t index = static_cast<size_t>(slot);
		currentSong->loopStationRevisionHistory[index].appendCurrent(*clip);
		auto const& runtime = engine_->track(slot);
		// Use lastSwungTickActioned (scheduler tick) as the base, since resumePlayback
		// uses it to compute sequenceSyncStartedAtTick.
		uint32_t currentTick = playbackHandler.lastSwungTickActioned;
		int32_t position;
		if (overdub) {
			// For overdub: continue from current phase in the existing loop
			uint32_t logicalPhase = loopPhaseAt(currentTick, runtime.playbackStartTick, targetLength);
			position = static_cast<int32_t>(
			    samplePhaseForLoopPhase(logicalPhase, clip->loopStationSampleZeroTicks, targetLength));
		}
		else {
			// For initial recording: start playback from the beginning (position 0)
			// The engine sets playbackStartTick = captureStartTick, so logical phase 0 = loop start
			position = 0;
		}
		clip->setPos(modelStack, position, true);
		clip->resumePlayback(modelStack, true);
	}
	return {.accepted = accepted, .committedLength = accepted ? targetLength : 0};
}

void LoopStationView::abortCapture(SlotId slot, bool overdub) {
	recorderBudget_.release(slot);
	AudioClip* clip = backingClip(slot);
	if (!clip) {
		return;
	}
	clip->abortRecording();
}

void LoopStationView::phraseCleared(SlotId slot) {
	auto index = static_cast<size_t>(slot);
	if (AudioClip* clip = backingClip(slot)) {
		clip->loopStationSampleZeroTicks = 0;
	}
	currentSong->loopStationRevisionHistory[index].clear();
}

bool LoopStationView::navigateHistory(SlotId slot, bool redo, bool& targetHasPhrase) {
	AudioClip* clip = backingClip(slot);
	if (!clip) {
		return false;
	}
	auto& history = currentSong->loopStationRevisionHistory[static_cast<size_t>(slot)];
	int32_t oldLength = clip->loopLength;
	::Error error = history.navigate(*clip, redo);
	if (error != ::Error::NONE) {
		display->displayError(error);
		return false;
	}
	targetHasPhrase = history.hasCurrent();

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	auto* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	if (oldLength != clip->loopLength) {
		clip->lengthChanged(modelStack, oldLength, nullptr);
	}
	clip->renderData.xScroll = -1;
	return true;
}

void LoopStationView::stateChanged(SlotId slot, TrackState state) {
	AudioClip* clip = backingClip(slot);
	if (state == TrackState::ERROR) {
		switch (engine_->track(slot).error) {
		case deluge::loop_station::Error::BACKING_UNAVAILABLE:
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_LOOP_STATION_BACKING_UNAVAILABLE));
			break;
		case deluge::loop_station::Error::MAX_FILE_SIZE:
			display->displayError(::Error::MAX_FILE_SIZE_REACHED);
			break;
		case deluge::loop_station::Error::SD_CARD:
			display->displayError(::Error::SD_CARD);
			break;
		case deluge::loop_station::Error::RESOURCE_LIMIT:
		case deluge::loop_station::Error::ALLOCATION:
			display->displayError(::Error::INSUFFICIENT_RAM);
			break;
		case deluge::loop_station::Error::SOURCE_REMOVED:
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_LOOP_STATION_SOURCE_UNAVAILABLE));
			break;
		case deluge::loop_station::Error::NONE:
			break;
		}
		refreshMonitoring();
		updateUndoLed();
		return;
	}
	if (!clip) {
		return;
	}
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	auto* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	if (state == TrackState::STOPPED || state == TrackState::EMPTY) {
		clip->activeIfNoSolo = false;
		clip->expectNoFurtherTicks(currentSong, true);
		refreshMonitoring();
		updateUndoLed();
		return;
	}
	if (state == TrackState::PLAYING) {
		bool needsResume = !currentSong->isClipActive(clip) || !clip->voiceSample;
		clip->activeIfNoSolo = true;
		clip->output->setActiveClip(modelStack);
		if (needsResume && playbackHandler.isEitherClockActive() && !clip->isEmpty(false)) {
			// Use lastSwungTickActioned (scheduler tick) as the base, since resumePlayback
			// uses it to compute sequenceSyncStartedAtTick.
			uint32_t currentTick = playbackHandler.lastSwungTickActioned;
			uint32_t playbackStart = engine_->track(slot).playbackStartTick;
			uint32_t length = std::max<int32_t>(1, clip->loopLength);
			uint32_t logicalPhase = loopPhaseAt(currentTick, playbackStart, length);
			// Translate logical phase to physical sample position using the recorded sample-zero offset.
			clip->setPos(
			    modelStack,
			    static_cast<int32_t>(samplePhaseForLoopPhase(logicalPhase, clip->loopStationSampleZeroTicks, length)),
			    true);
			clip->resumePlayback(modelStack, true);
		}
		refreshMonitoring();
		updateUndoLed();
		return;
	}
	if (state == TrackState::RECORDING || state == TrackState::OVERDUBBING) {
		bool needsPhraseResume = state == TrackState::OVERDUBBING && !clip->voiceSample && !clip->isEmpty(false);
		clip->activeIfNoSolo = true;
		clip->output->setActiveClip(modelStack);
		// The output recorder is only a tap; it must never replace playback of
		// the original phrase. Recover the voice at its current musical phase if
		// it was unavailable when Overdub began.
		if (needsPhraseResume && playbackHandler.isEitherClockActive()) {
			// Use lastSwungTickActioned (scheduler tick) as the base, since resumePlayback
			// uses it to compute sequenceSyncStartedAtTick. Using musicalPositionQ16()
			// would cause a phase mismatch.
			uint32_t currentTick = playbackHandler.lastSwungTickActioned;
			uint32_t playbackStart = engine_->track(slot).playbackStartTick;
			uint32_t length = std::max<int32_t>(1, clip->loopLength);
			uint32_t logicalPhase = loopPhaseAt(currentTick, playbackStart, length);
			// Translate logical phase to physical sample position using the recorded sample-zero offset.
			clip->setPos(
			    modelStack,
			    static_cast<int32_t>(samplePhaseForLoopPhase(logicalPhase, clip->loopStationSampleZeroTicks, length)),
			    true);
			clip->resumePlayback(modelStack, true);
		}
		// Non-empty Audio Clips overdub by recording their own output. The
		// physical input must already be monitored into that output before the
		// first recorder buffer is rendered.
		refreshMonitoring();
		updateUndoLed();
		return;
	}
	updateUndoLed();
}

void LoopStationView::playbackGainChanged(SlotId slot, int32_t gain) {
	if (AudioClip* clip = backingClip(slot)) {
		static_cast<AudioOutput*>(clip->output)->setLoopStationPlaybackGain(gain);
	}
}

uint32_t LoopStationView::playbackPhase(SlotId slot, uint32_t tickPhase) {
	AudioClip* clip = backingClip(slot);
	if (!clip || !clip->voiceSample || !clip->sampleHolder.audioFile || clip->loopLength <= 0) {
		return tickPhase;
	}
	auto* sample = static_cast<Sample*>(clip->sampleHolder.audioFile);
	uint32_t duration = std::max<uint32_t>(1, clip->sampleHolder.getDurationInSamples(true));
	int32_t playSample = clip->voiceSample->getPlaySample(sample, &clip->guide);
	int64_t relative = static_cast<int64_t>(playSample) - clip->sampleHolder.startPos;
	if (relative < 0) {
		relative = 0;
	}
	uint32_t samplePhase = static_cast<uint32_t>(static_cast<uint64_t>(relative) % duration);
	uint32_t samplePhaseTicks = static_cast<uint32_t>(static_cast<uint64_t>(samplePhase) * clip->loopLength / duration);
	return (samplePhaseTicks + clip->loopStationSampleZeroTicks) % clip->loopLength;
}

void LoopStationView::applyTrackSource(SlotId slot) {
	ensureBackingClip(slot);
}

void LoopStationView::refreshMonitoring() {
	std::array<AudioOutput*, kTrackCount> outputs{};
	for (size_t slotIndex = 0; slotIndex < kTrackCount; ++slotIndex) {
		for (int32_t i = 0; i < currentSong->sessionClips.getNumElements(); ++i) {
			Clip* clip = currentSong->sessionClips.getClipAtIndex(i);
			if (clip->type == ClipType::AUDIO && static_cast<AudioClip*>(clip)->loopStationSlotId == slotIndex + 1) {
				auto* audioClip = static_cast<AudioClip*>(clip);
				outputs[slotIndex] = static_cast<AudioOutput*>(audioClip->output);
				outputs[slotIndex]->setLoopStationMonitoring(false);
				auto const& settings = currentSong->loopStation.tracks[slotIndex].settings;
				auto const& runtime = engine_->track(static_cast<SlotId>(slotIndex));
				if (audioClip->recorder && audioClip->recorder->captureOutputPreFX) {
					// Recorder captures physical input during active overdub AND during background finish
					audioClip->recorder->captureOutputInputActive = shouldRecordPhysicalOverdubInput(runtime);
				}
				break;
			}
		}
	}

	monitoringCaptureMask_ = capturingMask();
	std::array<bool, 4> physicalSourceClaimed{};
	// Capturing tracks get first claim on a physical source. The selected track
	// then supplies an idle return when it is not already capturing.
	for (uint8_t pass = 0; pass < 2; ++pass) {
		for (size_t slotIndex = 0; slotIndex < kTrackCount; ++slotIndex) {
			AudioOutput* output = outputs[slotIndex];
			bool capturing = monitoringCaptureMask_ & (1u << slotIndex);
			bool selected = selectedSlot_ && slotIndex == static_cast<size_t>(*selectedSlot_);
			bool wantedForPass = pass == 0 ? capturing : !capturing && selected;
			if (!output || !wantedForPass) {
				continue;
			}

			auto const& settings = currentSong->loopStation.tracks[slotIndex].settings;
			bool physical = isPhysicalSource(settings.source);
			bool enabled = monitoringActive_ && settings.monitoring && physical
			               && currentSong->isOutputActiveInArrangement(output);
			if (enabled) {
				size_t sourceIndex = static_cast<size_t>(settings.source);
				enabled = !physicalSourceClaimed[sourceIndex];
				physicalSourceClaimed[sourceIndex] = true;
			}
			output->setLoopStationMonitoring(enabled, settings.transpose, settings.transposeCents);
		}
	}
}

void LoopStationView::disableMonitoring() {
	monitoringActive_ = false;
	refreshMonitoring();
	playbackHandler.recording = recordingModeBefore_;
}

void LoopStationView::clearBackingClip(SlotId slot) {
	AudioClip* clip = ensureBackingClip(slot);
	if (!clip)
		return;
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	auto* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	clip->clear(nullptr, modelStack, true, true);
	clip->activeIfNoSolo = false;
}

void LoopStationView::dispatchCentreGesture(SlotId slot, uint32_t pressSample) {
	if (engine_->track(slot).state == TrackState::EMPTY && !ensureBackingClip(slot)) {
		display->displayError(::Error::INSUFFICIENT_RAM);
		return;
	}
	// AudioClip::resumePlayback requires an active clock. Starting a stopped
	// transport establishes it before the state callback starts the sample.
	if (!playbackHandler.isEitherClockActive()) {
		playbackHandler.setupPlaybackUsingInternalClock(0, false, false, true);
	}
	auto now = transport();
	uint32_t gestureMs = static_cast<uint32_t>((static_cast<uint64_t>(pressSample) * 1000) / kSampleRate);
	engine_->centrePressed(slot, now, gestureMs);
	updateRecordingMode();
	refreshMonitoring();
	playbackHandler.expectEvent();
}

ActionResult LoopStationView::padAction(int32_t x, int32_t y, int32_t velocity) {
	if (x < 0 || x >= kDisplayWidth || y < 0 || y >= kDisplayHeight || !engine_) {
		return ActionResult::DEALT_WITH;
	}
	auto slot = Geometry::slotAt(x, y);
	auto origin = Geometry::origin(slot);
	uint8_t localX = x - origin.x;
	uint8_t localY = y - origin.y;
	if (!Geometry::isCentre(localX, localY)) {
		size_t index = static_cast<size_t>(slot);
		uint16_t bit = 0;
		for (uint8_t p = 0; p < kCellBorderPadCount; ++p) {
			if (Geometry::kClockwiseBorder[p] == PadCoordinate{localX, localY}) {
				bit = static_cast<uint16_t>(1u << p);
				break;
			}
		}
		if (!bit) {
			return ActionResult::DEALT_WITH;
		}
		if (velocity) {
			if (borderHeldMask_[index] == 0) {
				if (selectedSlot_ && *selectedSlot_ == slot) {
					selectedSlot_.reset();
				}
				else {
					selectedSlot_ = slot;
					// Selection owns the idle monitoring return. Create the normal
					// backing Audio Clip now so selecting an unused slot can monitor
					// immediately without requiring SHIFT + centre first.
					if (!ensureBackingClip(slot)) {
						selectedSlot_.reset();
						display->displayError(::Error::INSUFFICIENT_RAM);
					}
				}
				updateModControllableTarget();
				refreshMonitoring();
				updateUndoLed();
				uiNeedsRendering(this, 0xFFFFFFFF, 0);
			}
			borderHeldMask_[index] |= bit;
		}
		else {
			borderHeldMask_[index] &= ~bit;
		}
		return ActionResult::DEALT_WITH;
	}

	size_t index = static_cast<size_t>(slot);
	uint8_t bit = 1u << ((localY - 1) * 2 + localX - 1);
	if (velocity) {
		if (centreHeldMask_[index] == 0) {
			centrePressSample_[index] = AudioEngine::audioSampleTimer;
			centreGestureActive_[index] = true;
			centreGestureDispatched_[index] = false;
			// Record, stop-record, Play->Overdub, and Overdub->Play are
			// performance gestures, so action them on the first physical press.
			// A stopped phrase is deferred until release because the same gesture
			// becomes Clear when held for two seconds. SHIFT gestures are menus.
			if (centreGestureActsOnPress(engine_->track(slot).state, Buttons::isShiftButtonPressed())) {
				dispatchCentreGesture(slot, centrePressSample_[index]);
				centreGestureDispatched_[index] = true;
				uiNeedsRendering(this, 0xFFFFFFFF, 0);
			}
		}
		centreHeldMask_[index] |= bit;
		return ActionResult::DEALT_WITH;
	}

	centreHeldMask_[index] &= ~bit;
	if (centreHeldMask_[index] != 0 || !centreGestureActive_[index]) {
		return ActionResult::DEALT_WITH;
	}
	centreGestureActive_[index] = false;
	bool dispatchedOnPress = centreGestureDispatched_[index];
	centreGestureDispatched_[index] = false;
	uint32_t heldSamples = AudioEngine::audioSampleTimer - centrePressSample_[index];
	if (!dispatchedOnPress && heldSamples >= kDeleteHoldDurationSamples
	    && engine_->track(slot).state == TrackState::STOPPED) {
		clearBackingClip(slot);
		engine_->clearHeld(slot);
	}
	else if (!dispatchedOnPress && Buttons::isShiftButtonPressed()) {
		AudioClip* clip = ensureBackingClip(slot);
		if (!clip) {
			display->displayError(::Error::INSUFFICIENT_RAM);
			return ActionResult::DEALT_WITH;
		}
		currentSong->setCurrentClip(clip);
		deluge::gui::menu_item::loop_station::selectTrack(slot);
		clipMenuContext_ = true;
		display->setNextTransitionDirection(1);
		if (soundEditor.setup(clip, &deluge::gui::menu_item::loop_station::trackSettingsMenu)) {
			openUI(&soundEditor);
		}
		else {
			clipMenuContext_ = false;
		}
	}
	else if (!dispatchedOnPress) {
		dispatchCentreGesture(slot, centrePressSample_[index]);
	}
	uiNeedsRendering(this, 0xFFFFFFFF, 0);
	return ActionResult::DEALT_WITH;
}

ActionResult LoopStationView::buttonAction(deluge::hid::Button button, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;
	if (!engine_) {
		return ActionResult::DEALT_WITH;
	}
	if (button == CROSS_SCREEN_EDIT || button == SESSION_VIEW) {
		if (!on) {
			if (inCardRoutine) {
				return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
			}
			requestExit();
		}
		return ActionResult::DEALT_WITH;
	}
	if (button == BACK) {
		if (on) {
			if (selectedSlot_) {
				if (Buttons::isShiftButtonPressed()) {
					engine_->redo(*selectedSlot_);
				}
				else {
					engine_->undo(*selectedSlot_);
				}
				refreshMonitoring();
			}
			updateUndoLed();
			uiNeedsRendering(this, 0xFFFFFFFF, 0);
		}
		return ActionResult::DEALT_WITH;
	}
	if (button == SELECT_ENC && on && !Buttons::isShiftButtonPressed()) {
		if (inCardRoutine) {
			return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
		}
		clipMenuContext_ = false;
		display->setNextTransitionDirection(1);
		if (soundEditor.setup(nullptr, &deluge::gui::menu_item::loop_station::globalSettingsMenu)) {
			openUI(&soundEditor);
		}
		return ActionResult::DEALT_WITH;
	}
	// Preserve the standard global controls while Loop Station is the root UI.
	// In particular, Shift + Tap Tempo toggles the metronome and Tap Tempo keeps
	// its normal behavior.
	return view.buttonAction(button, on, inCardRoutine);
}

void LoopStationView::requestExit() {
	exitRequested_ = true;
	bool ready = engine_->requestExit(transport());
	updateRecordingMode();
	if (ready) {
		disableMonitoring();
		changeRootUI(&sessionView);
	}
	else {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_LOOP_STATION_COMMITTING));
	}
}

RGB LoopStationView::palette(PadColour colour) const {
	using namespace deluge::gui::colours;
	switch (colour) {
	case PadColour::RED:
		return red;
	case PadColour::ORANGE:
		return orange;
	case PadColour::YELLOW:
		return yellow;
	case PadColour::GREEN:
		return green;
	case PadColour::CYAN:
		return cyan_full;
	case PadColour::BLUE:
		return blue;
	case PadColour::PURPLE:
		return purple;
	case PadColour::MAGENTA:
		return magenta_full;
	case PadColour::WHITE:
		return white_full;
	}
	return white_full;
}

RGB LoopStationView::destinationColour(PendingAction action) const {
	using namespace deluge::gui::colours;
	switch (action) {
	case PendingAction::START_RECORDING:
		return red;
	case PendingAction::START_OVERDUB:
		return orange;
	case PendingAction::FINISH_RECORDING:
		return currentSong->loopStation.global.recordAction == RecordAction::RECORD_OVERDUB_PLAY ? orange : green;
	case PendingAction::FINISH_OVERDUB:
	case PendingAction::RETRIGGER:
	case PendingAction::RESUME:
		return green;
	case PendingAction::STOP:
		return green.dim(3);
	case PendingAction::NONE:
		return black;
	}
	return black;
}

bool LoopStationView::renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                                     uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool) {
	if (!image || !engine_) {
		return true;
	}
	auto now = transport();
	uint64_t musicalPosition = musicalPositionQ16();
	uint32_t quarter = std::max<uint32_t>(1, currentSong->getQuarterNoteLength());
	uint16_t beatPulse = Geometry::beatPulseBrightness(musicalPosition, quarter);
	bool rapidBlink = ((now.tick / std::max<uint32_t>(1, quarter / 8)) & 1) == 0;

	for (size_t i = 0; i < kTrackCount; ++i) {
		auto slot = static_cast<SlotId>(i);
		auto origin = Geometry::origin(slot);
		auto const& runtime = engine_->track(slot);
		auto const& settings = currentSong->loopStation.tracks[i].settings;
		RGB trackColour = palette(settings.progressColour);
		RGB dimGlobal = palette(currentSong->loopStation.global.dimBorderColour).dim(3);
		RGB centre = deluge::gui::colours::grey;
		RGB border = dimGlobal;
		bool progress = false;
		bool deleting = runtime.state == TrackState::STOPPED && centreGestureActive_[i] && !centreGestureDispatched_[i]
		                && !Buttons::isShiftButtonPressed();
		auto deleteProgress = deleting
		                          ? Geometry::linearProgressFill(AudioEngine::audioSampleTimer - centrePressSample_[i],
		                                                         kDeleteHoldDurationSamples)
		                          : Geometry::ProgressFill{};

		if (runtime.event.pending()) {
			centre = rapidBlink ? destinationColour(runtime.event.action) : black;
			border = centre;
		}
		else {
			switch (runtime.state) {
			case TrackState::EMPTY:
				break;
			case TrackState::RECORDING:
				centre = deluge::gui::colours::red;
				border = RGB::blend(deluge::gui::colours::red, deluge::gui::colours::red.dim(3), beatPulse);
				break;
			case TrackState::OVERDUBBING:
				centre = deluge::gui::colours::orange;
				border = RGB::blend(deluge::gui::colours::orange, deluge::gui::colours::orange.dim(3), beatPulse);
				break;
			case TrackState::PLAYING:
				centre = deluge::gui::colours::green;
				progress = true;
				break;
			case TrackState::STOPPED:
				centre = deluge::gui::colours::green.dim(3);
				border = trackColour.dim(3);
				break;
			case TrackState::COMMITTING:
				centre = rapidBlink ? deluge::gui::colours::orange : black;
				border = centre;
				break;
			case TrackState::ERROR:
				centre = rapidBlink ? deluge::gui::colours::red : black;
				border = deluge::gui::colours::red.dim(3);
				break;
			}
		}

		uint64_t playbackStartQ16 = static_cast<uint64_t>(runtime.playbackStartTick) << 16;
		auto progressFill = Geometry::progressFill(
		    musicalPosition >= playbackStartQ16 ? musicalPosition - playbackStartQ16 : 0, runtime.phraseLength);
		for (uint8_t p = 0; p < kCellBorderPadCount; ++p) {
			auto local = Geometry::kClockwiseBorder[p];
			uint8_t x = origin.x + local.x;
			uint8_t y = origin.y + local.y;
			if (whichRows & (1u << y)) {
				if (deleting && p < deleteProgress.fullPads) {
					image[y][x] = deluge::gui::colours::red;
				}
				else if (deleting && p == deleteProgress.fullPads && p < kCellBorderPadCount) {
					image[y][x] = RGB::blend(deluge::gui::colours::red, border, deleteProgress.leadingBrightness);
				}
				else if (progress && p < progressFill.fullPads) {
					image[y][x] = trackColour;
				}
				else if (progress && p == progressFill.fullPads && p < kCellBorderPadCount) {
					image[y][x] = RGB::blend(trackColour, border, progressFill.leadingBrightness);
				}
				else {
					image[y][x] = border;
				}
				if (!deleting && selectedSlot_ && *selectedSlot_ == slot && runtime.state != TrackState::RECORDING
				    && runtime.state != TrackState::OVERDUBBING) {
					// White lift keeps state and progress hues recognizable while
					// making the complete selected border visibly brighter.
					image[y][x] = RGB::blend(deluge::gui::colours::white_full, image[y][x], 24576);
				}
				if (occupancyMask)
					occupancyMask[y][x] = 64;
			}
		}
		for (uint8_t localY = 1; localY <= 2; ++localY) {
			for (uint8_t localX = 1; localX <= 2; ++localX) {
				uint8_t x = origin.x + localX;
				uint8_t y = origin.y + localY;
				if (whichRows & (1u << y)) {
					image[y][x] = centre;
					if (occupancyMask)
						occupancyMask[y][x] = 64;
				}
			}
		}
	}
	return true;
}

bool LoopStationView::renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) {
	if (image) {
		for (uint8_t y = 0; y < kDisplayHeight; ++y) {
			if (!(whichRows & (1u << y)))
				continue;
			for (uint8_t x = kDisplayWidth; x < kDisplayWidth + kSideBarWidth; ++x) {
				image[y][x] = black;
				if (occupancyMask)
					occupancyMask[y][x] = 64;
			}
		}
	}
	return true;
}

void LoopStationView::renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	canvas.clearAreaExact(0, OLED_MAIN_TOPMOST_PIXEL, OLED_MAIN_WIDTH_PIXELS - 1, OLED_MAIN_HEIGHT_PIXELS - 1);
	int32_t y = OLED_MAIN_TOPMOST_PIXEL + (OLED_MAIN_HEIGHT_PIXELS - OLED_MAIN_TOPMOST_PIXEL - kTextTitleSizeY) / 2;
	canvas.drawStringCentred(l10n::get(l10n::String::STRING_FOR_LOOP_STATION), y, kTextTitleSpacingX, kTextTitleSizeY);
}

void LoopStationView::graphicsRoutine() {
	if (!engine_)
		return;
	processMusicalTick();
	indicator_leds::setLedState(IndicatorLED::RECORD, engine_->anyCapturing());
	uiNeedsRendering(this, 0xFFFFFFFF, 0);
	if (exitRequested_ && engine_->exitReady()) {
		disableMonitoring();
		changeRootUI(&sessionView);
	}
}

void LoopStationView::processMusicalTick() {
	if (!engine_)
		return;
	auto now = transport();
	engine_->processTick(now);
	for (size_t i = 0; i < kTrackCount; ++i) {
		auto slot = static_cast<SlotId>(i);
		auto const& runtime = engine_->track(slot);
		if (runtime.state != TrackState::RECORDING && runtime.state != TrackState::OVERDUBBING
		    && !runtime.finishingOverdub) {
			continue;
		}
		AudioClip* clip = backingClip(slot);
		deluge::loop_station::Error failure = deluge::loop_station::Error::NONE;
		if (!clip || !clip->recorder)
			failure = deluge::loop_station::Error::ALLOCATION;
		else if (clip->recorder->hadCardError)
			failure = deluge::loop_station::Error::SD_CARD;
		else if (clip->recorder->reachedMaxFileSize)
			failure = deluge::loop_station::Error::MAX_FILE_SIZE;
		else if (clip->recorder->captureOutputBackingMissing) {
			failure = deluge::loop_station::Error::BACKING_UNAVAILABLE;
		}
		else if (clip->recorder->status == RecorderStatus::ABORTED) {
			failure = deluge::loop_station::Error::ALLOCATION;
		}
		if (failure != deluge::loop_station::Error::NONE)
			engine_->notifyFailure(slot, failure);
	}
	updateRecordingMode();
	if (monitoringCaptureMask_ != capturingMask()) {
		refreshMonitoring();
	}
	uint32_t ticks = engine_->ticksUntilNextEvent(now.tick);
	if (ticks != std::numeric_limits<uint32_t>::max()) {
		playbackHandler.swungTicksTilNextEvent = std::min<int32_t>(playbackHandler.swungTicksTilNextEvent, ticks);
	}
}

void LoopStationView::playbackEnded() {
	uiNeedsRendering(this, 0xFFFFFFFF, 0);
}
