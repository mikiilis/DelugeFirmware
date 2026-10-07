#include "loop_station/diagnostics.h"
/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gui/views/loop_station_view.h"
#include "loop_station/commit.h"

#include "gui/colour/palette.h"
#include "gui/context_menu/clear_song.h"
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
#include "hid/led/pad_leds.h"
#include "loop_station/scheduler.h"
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
constexpr uint8_t kGroupControlsRow = kDisplayHeight - 1;
constexpr uint8_t kStartAllX = kDisplayWidth;
constexpr uint8_t kStopAllX = kDisplayWidth + 1;

deluge::loop_station::Error recorderError(::Error error) {
	switch (error) {
	case ::Error::BUG:
		return deluge::loop_station::Error::INTERNAL;
	case ::Error::INSUFFICIENT_RAM:
	case ::Error::INSUFFICIENT_RAM_FOR_FOLDER_CONTENTS_SIZE:
	case ::Error::OUT_OF_BUFFER_SPACE:
		return deluge::loop_station::Error::ALLOCATION;
	case ::Error::MAX_FILE_SIZE_REACHED:
		return deluge::loop_station::Error::MAX_FILE_SIZE;
	case ::Error::SD_CARD_FULL:
		return deluge::loop_station::Error::SD_CARD_FULL;
	case ::Error::SD_CARD_NOT_PRESENT:
		return deluge::loop_station::Error::SD_CARD_NOT_PRESENT;
	case ::Error::SD_CARD_NO_FILESYSTEM:
		return deluge::loop_station::Error::SD_CARD_NO_FILESYSTEM;
	case ::Error::WRITE_PROTECTED:
		return deluge::loop_station::Error::WRITE_PROTECTED;
	case ::Error::NONE:
	case ::Error::SD_CARD:
	default:
		return deluge::loop_station::Error::SD_CARD;
	}
}

} // namespace

PLACE_SDRAM_BSS LoopStationView loopStationView{};

bool LoopStationView::opened() {
	// Reopening must never overwrite a recorder transaction which yielded to UI work.
	bool pending = std::any_of(pendingCommitRecorders_.begin(), pendingCommitRecorders_.end(),
	                           [](auto* recorder) { return recorder != nullptr; });
	if (engine_ && (engine_->anyCapturing() || pending)) {
		if (&engine_->model() != &currentSong->loopStation)
			return false;
		focusRegained();
		return true;
	}
	recordingModeBefore_ = playbackHandler.recording;
	pendingCommitRecorders_.fill(nullptr);
	engine_.emplace(currentSong->loopStation, static_cast<EngineDelegate*>(this));
	exitRequested_ = false;
	deferredGlobalButton_.reset();
	deferredGlobalButtonReleased_ = false;
	deferredGlobalShift_ = false;
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
	// Song View's playback cursor is a shared PadLED overlay rather than part of
	// its base grid image. Remove its last stored position before rendering Loop
	// Station, including when returning from a settings menu.
	PadLEDs::clearTickSquares(false);
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
	uint64_t position = musicalPositionQ16();
	uint32_t tick = static_cast<uint32_t>(position >> 16);
	uint32_t bar = std::max<uint32_t>(1, currentSong->getBarLength());
	return {.tick = tick,
	        .barLength = bar,
	        .eighthNoteLength = std::max<uint32_t>(1, currentSong->getQuarterNoteLength() / 2),
	        .playing = playbackHandler.playbackState != 0,
	        .arrangement = playbackHandler.playbackState && currentPlaybackMode == &arrangement,
	        .afterTickBoundary = (position & 0xFFFF) != 0,
	        .metronomeOffset = static_cast<uint32_t>(playbackHandler.metronomeOffset),
	        .fractionQ16 = static_cast<uint16_t>(position)};
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
		            || runtime.finishingOverdub;
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
			auto& binding = currentSong->loopStation.tracks[static_cast<size_t>(slot)];
			if (!binding.transposePresent && (audioClip->sampleHolder.transpose || audioClip->sampleHolder.cents)) {
				writeTrackParameter(currentSong->loopStation, slot, TrackParameter::TRANSPOSE,
				                    audioClip->sampleHolder.transpose * 100 + audioClip->sampleHolder.cents);
			}
			// Loop Station transpose is a track setting. Sample metadata must stay
			// neutral so another overdub never bakes or reapplies the setting.
			audioClip->sampleHolder.transpose = 0;
			audioClip->sampleHolder.cents = 0;
			audioClip->sampleHolder.recalculateNeutralPhaseIncrement();
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

deluge::loop_station::Error LoopStationView::preflight(SlotId slot, bool overdub) {
	AudioClip* clip = ensureBackingClip(slot);
	if (!clip)
		return deluge::loop_station::Error::ALLOCATION;
	if (clip->recorder)
		return deluge::loop_station::Error::RESOURCE_LIMIT;
	if (generationCount_[static_cast<size_t>(slot)] == kPendingGenerationCapacity)
		return deluge::loop_station::Error::RESOURCE_LIMIT;
	if (clip->loopStationPlaybackRecorder
	    && clip->loopStationPlaybackRecorder->loopGenerationDepth() >= kPendingGenerationCapacity)
		return deluge::loop_station::Error::RESOURCE_LIMIT;
	if (overdub == clip->isEmpty(false))
		return overdub ? deluge::loop_station::Error::BACKING_UNAVAILABLE : deluge::loop_station::Error::INTERNAL;
	auto* output = static_cast<AudioOutput*>(clip->output);
	bool sourceValid =
	    output->inputChannel > AudioInputChannel::NONE
	    && (output->inputChannel != AudioInputChannel::SPECIFIC_OUTPUT || output->getOutputRecordingFrom());
	if (!sourceValid)
		return deluge::loop_station::Error::SOURCE_REMOVED;
	auto source = currentSong->loopStation.tracks[static_cast<size_t>(slot)].settings.source;
	if (!AudioEngine::hasRecorderChannelCapacity(recorderChannelCount(source, overdub)))
		return deluge::loop_station::Error::RESOURCE_LIMIT;
	return output->prepareLoopStationPitchShifter() == ::Error::NONE ? deluge::loop_station::Error::NONE
	                                                                 : deluge::loop_station::Error::ALLOCATION;
}

deluge::loop_station::Error LoopStationView::beginCapture(SlotId slot, bool overdub, uint32_t tick) {
	AudioClip* clip = backingClip(slot);
	OwnershipScope ownership({reinterpret_cast<uintptr_t>(currentSong), reinterpret_cast<uintptr_t>(clip),
	                          reinterpret_cast<uintptr_t>(clip ? clip->recorder : nullptr),
	                          reinterpret_cast<uintptr_t>(pendingCommitRecorders_[static_cast<size_t>(slot)]),
	                          static_cast<uint32_t>(slot), 1});
	if (!clip || clip->recorder) {
		return clip ? deluge::loop_station::Error::RESOURCE_LIMIT : deluge::loop_station::Error::BACKING_UNAVAILABLE;
	}

	size_t index = static_cast<size_t>(slot);
	auto reject = [&](deluge::loop_station::Error error) { return error; };
	// Reserve the append before creating the recorder, and retain the active
	// revision before an overdub. Allocation or SD failure can therefore never
	// strand the clip after its old Sample has been released.
	if (!currentSong->loopStationRevisionHistory[index].prepareAppend(*clip, overdub && !generationCount_[index],
	                                                                  generationCount_[index])) {
		return reject(deluge::loop_station::Error::ALLOCATION);
	}
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	auto* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	clip->activeIfNoSolo = true;
	clip->output->setActiveClip(modelStack);

	if (overdub && !clip->isEmpty(false)) {
		if (!clip->voiceSample && !clip->loopStationPlaybackRecorder)
			clip->resumePlayback(modelStack, true);
		if (!clip->voiceSample && !clip->loopStationPlaybackRecorder)
			return reject(deluge::loop_station::Error::BACKING_UNAVAILABLE);
		auto* sample = static_cast<Sample*>(clip->sampleHolder.audioFile);
		uint32_t frames = clip->sampleHolder.getDurationInSamples(true);
		int64_t frame = clip->loopStationPlaybackRecorder
		                    ? clip->loopStationPlaybackRecorder->loopPlaybackCursor()
		                    : clip->voiceSample->getPlaySample(sample, &clip->guide) - clip->sampleHolder.startPos;
		overdubStartFrames_[index] =
		    (std::max<int64_t>(0, frame) + clip->loopStationPhaseZeroFrames()) % std::max<uint32_t>(1, frames);
	}

	playbackHandler.recording = RecordingMode::NORMAL;
	::Error error = clip->beginLinearRecording(modelStack, 0);
	if (error != ::Error::NONE) {
		if (clip->recorder)
			clip->abortRecording();
		return reject(clip->recorderAdmissionDenied ? deluge::loop_station::Error::RESOURCE_LIMIT
		                                            : recorderError(error));
	}
	// Every Loop Station WAV is a fixed-gain dry revision. Explicitly disable
	// normalization and file alteration for first takes as well as overdubs.
	clip->recorder->allowFileAlterationAfter = false;
#if ALPHA_OR_BETA_VERSION
	clip->recorder->diagnosticLoopStationSlot = static_cast<uint8_t>(index);
#endif
	clip->recorder->allowNormalization = false;
	// Keep writer ownership until the WAV is closed; sealed writers no longer
	// consume the capture-channel budget.
	clip->recorder->autoDeleteWhenDone = false;
	clip->recorder->suppressErrorPopup = true;
	if (overdub && clip->recorder) {
		// Capture stored audio plus monitored input before track FX and without
		// normalization. The recorder captures the clip output (loop) + input.
		clip->recorder->captureOutputPreFX = true;
		clip->recorder->captureOutputInputActive = true;
		uint32_t frames = std::max<uint32_t>(1, clip->sampleHolder.getDurationInSamples(true));
		uint32_t sourceFrame = (overdubStartFrames_[index] + frames - clip->loopStationPhaseZeroFrames()) % frames;
		auto* sourceRecorder = clip->loopStationPlaybackRecorder;
		if (!sourceRecorder && generationCount_[index]) {
			auto tail = (generationHead_[index] + generationCount_[index] - 1) % kPendingGenerationCapacity;
			auto* candidate = generations_[index][tail].recorder;
			if (candidate && candidate->sample == clip->sampleHolder.audioFile)
				sourceRecorder = candidate;
		}
		auto result = clip->recorder->setupLoopOverdub(frames, static_cast<Sample*>(clip->sampleHolder.audioFile),
		                                               clip->sampleHolder.startPos, sourceFrame, sourceRecorder);
		if (result != ::Error::NONE) {
			clip->abortRecording();
			return reject(recorderError(result));
		}
		clip->setLoopStationPlaybackRecorder(clip->recorder);
	}
	refreshMonitoring();
	return deluge::loop_station::Error::NONE;
}

CaptureFinish LoopStationView::finishCapture(SlotId slot, bool overdub, uint32_t length, bool crop) {
	AudioClip* clip = backingClip(slot);
	OwnershipScope ownership({reinterpret_cast<uintptr_t>(currentSong), reinterpret_cast<uintptr_t>(clip),
	                          reinterpret_cast<uintptr_t>(clip ? clip->recorder : nullptr),
	                          reinterpret_cast<uintptr_t>(pendingCommitRecorders_[static_cast<size_t>(slot)]),
	                          static_cast<uint32_t>(slot), 2});
	if (!clip || !clip->recorder) {
		return {.accepted = false,
		        .error = overdub ? deluge::loop_station::Error::BACKING_UNAVAILABLE
		                         : deluge::loop_station::Error::ALLOCATION};
	}
	SampleRecorder* recorder = clip->recorder;
	uint32_t resumeFrame = 0;
	if (overdub)
		resumeFrame = recorder->loopPlaybackCursor();
	// Reject without publishing the provisional WAV, and preserve the exact
	// failure reason instead of presenting every capture failure as an SD error.
	auto reject = [clip](deluge::loop_station::Error error) {
		clip->abortRecording();
		return CaptureFinish{.accepted = false, .error = error};
	};
	if (recorder->reachedMaxFileSize)
		return reject(deluge::loop_station::Error::MAX_FILE_SIZE);
	if (recorder->terminalError != ::Error::NONE)
		return reject(recorderError(recorder->terminalError));
	if (recorder->hadCardError)
		return reject(deluge::loop_station::Error::SD_CARD);
	if (overdub && recorder->captureOutputBackingMissing) {
		return reject(deluge::loop_station::Error::BACKING_UNAVAILABLE);
	}
	if (recorder->status == RecorderStatus::ABORTED)
		return reject(deluge::loop_station::Error::ALLOCATION);
	if (!recorder->numSamplesCaptured && !(overdub && recorder->hasCapturedLoopInput()))
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
	// Ownership must be continuous through AudioClip detachment and any allocation/SD yields below.
	recorder->retainForCommit();
	clip->finishLinearRecording(modelStack, nullptr, 0);
	clip->name.set(&trackName);

	bool accepted = clip->sampleHolder.audioFile && (!overdub || clip->sampleHolder.audioFile != oldAudioFile);
	if (accepted) {
		// For overdubs starting mid-phrase, the new cumulative WAV's physical sample 0
		// corresponds to the overdub start position, not the original phrase start.
		// Track this so playback translates logical phrase position to physical sample position.
		clip->loopStationSampleZeroFrames = overdub ? overdubStartFrames_[static_cast<size_t>(slot)] : 0;
		clip->loopStationSampleZeroTicks = 0;

		clip->sampleHolder.transpose = 0;
		clip->sampleHolder.cents = 0;
		clip->sampleHolder.recalculateNeutralPhaseIncrement();

		size_t index = static_cast<size_t>(slot);
		auto tail = (generationHead_[index] + generationCount_[index]) % kPendingGenerationCapacity;
		auto& generation = generations_[index][tail];
		generation.recorder = recorder;
		generation.revision.capture(*clip);
		++generationCount_[index];
		pendingCommitRecorders_[index] = generations_[index][generationHead_[index]].recorder;

		auto const& runtime = engine_->track(slot);
		if (!runtime.stopAfterCapture && !overdub)
			clip->resumeLoopStationAtFrame(modelStack, resumeFrame);
	}
	if (!accepted) {
		recorder->releaseCommit(false);
	}
	return {.accepted = accepted,
	        .asynchronous = accepted,
	        .committedLength = accepted ? targetLength : 0,
	        .error = accepted ? deluge::loop_station::Error::NONE
	                          : (recorder->terminalError == ::Error::NONE ? deluge::loop_station::Error::ALLOCATION
	                                                                      : recorderError(recorder->terminalError))};
}

void LoopStationView::endOverdubInput(SlotId slot) {
	if (auto* clip = backingClip(slot); clip && clip->recorder)
		clip->recorder->stopLoopOverdubInput();
	refreshMonitoring();
}

bool LoopStationView::overdubReady(SlotId slot) {
	auto* clip = backingClip(slot);
	return clip && clip->recorder;
}

bool LoopStationView::playbackEnded(SlotId slot) {
	auto* clip = backingClip(slot);
	return clip
	       && (clip->loopStationPlaybackRecorder ? clip->loopStationPlaybackRecorder->loopPlaybackEnded()
	                                             : !clip->voiceSample && !clip->doingLateStart);
}

void LoopStationView::abortCapture(SlotId slot, bool overdub) {
	AudioClip* clip = backingClip(slot);
	if (!clip) {
		return;
	}
	auto* live = clip->loopStationPlaybackRecorder;
	auto* parent = live ? live->loopSourceRecorder() : nullptr;
	uint32_t resumeFrame = live ? live->loopSourcePlaybackFrame() : 0;
	bool parentProvidesLoopPlayback = parent && parent->isLoopOverdub();
	if (parentProvidesLoopPlayback)
		parent->setLoopPlaybackCursor(resumeFrame);
	clip->setLoopStationPlaybackRecorder(parentProvidesLoopPlayback ? parent : nullptr);
	clip->abortRecording();
	// A linear parent recorder is a source owner, not a Loop Station playback
	// provider. Fall back to the durable AudioClip at the source frame instead
	// of leaving the clip with no audible provider.
	if (live && !parentProvidesLoopPlayback && playbackHandler.isEitherClockActive() && !clip->isEmpty(false)) {
		char memory[MODEL_STACK_MAX_SIZE];
		auto* stack = setupModelStackWithTimelineCounter(memory, currentSong, clip);
		clip->unassignVoiceSample(false);
		clip->resumeLoopStationAtFrame(stack, resumeFrame, true);
	}
}

void LoopStationView::phraseCleared(SlotId slot) {
	auto index = static_cast<size_t>(slot);
	if (AudioClip* clip = backingClip(slot)) {
		clip->setLoopStationPlaybackRecorder(nullptr);
		clip->loopStationSampleZeroTicks = 0;
		clip->loopStationSampleZeroFrames = 0;
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
	clip->setLoopStationPlaybackRecorder(nullptr);
	targetHasPhrase = history.hasCurrent();

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	auto* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	if (oldLength != clip->loopLength) {
		clip->lengthChanged(modelStack, oldLength, nullptr);
	}
	clip->renderData.xScroll = -1;
	return true;
}

void LoopStationView::resumeCommittedClip(SlotId slot, uint32_t logicalSampleFrame) {
	AudioClip* clip = backingClip(slot);
	if (!clip || clip->isEmpty(false)) {
		if (clip) {
			clip->activeIfNoSolo = false;
			clip->expectNoFurtherTicks(currentSong, true);
		}
		return;
	}
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	auto* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	clip->activeIfNoSolo = true;
	clip->output->setActiveClip(modelStack);
	uint32_t frameCount = std::max<uint32_t>(1, clip->sampleHolder.getDurationInSamples(true));
	uint32_t sampleFrame =
	    sampleFrameForLogicalSampleFrame(logicalSampleFrame, clip->loopStationPhaseZeroFrames(), frameCount);
	clip->resumeLoopStationAtFrame(modelStack, sampleFrame, true);
}

uint32_t LoopStationView::currentLogicalSampleFrame(size_t index, AudioClip& clip) const {
	if (!clip.sampleHolder.audioFile)
		return 0;
	auto* sample = static_cast<Sample*>(clip.sampleHolder.audioFile);
	uint32_t frameCount = std::max<uint32_t>(1, clip.sampleHolder.getDurationInSamples(true));
	uint32_t sampleZeroFrame = clip.loopStationPhaseZeroFrames();
	if (clip.recorder && clip.recorder == clip.loopStationPlaybackRecorder && clip.recorder->isLoopOverdub())
		sampleZeroFrame = overdubStartFrames_[index];
	int64_t sampleFrame = clip.loopStationPlaybackRecorder ? clip.loopStationPlaybackRecorder->loopPlaybackCursor()
	                      : clip.voiceSample
	                          ? clip.voiceSample->getPlaySample(sample, &clip.guide) - clip.sampleHolder.startPos
	                          : 0;
	uint32_t physicalFrame = static_cast<uint32_t>(std::max<int64_t>(0, sampleFrame)) % frameCount;
	return logicalSampleFrameForSampleFrame(physicalFrame, sampleZeroFrame, frameCount);
}

void LoopStationView::releaseSlot(Song& song, size_t index) {
	if (!engine_ || &engine_->model() != &song.loopStation)
		return;
	// The caller has removed the binding/history. Clip destruction releases
	// any remaining reader or active-recorder ownership after this callback.
	discardGenerations(index);
	engine_->restoreBoundPhrase(static_cast<SlotId>(index), 0, false, false, 0, -1);
}

void LoopStationView::releaseSong(Song& song) {
	if (!engine_ || &engine_->model() != &song.loopStation)
		return;
	// Disable all callbacks before Song destruction starts yielding to audio.
	engine_.reset();
	for (size_t i = 0; i < pendingCommitRecorders_.size(); ++i) {
		discardGenerations(i);
		song.loopStationRevisionHistory[i].discardStaged();
	}
}

void LoopStationView::discardGenerations(size_t slot) {
	pendingCommitRecorders_[slot] = nullptr;
	while (generationCount_[slot]) {
		auto& entry = generations_[slot][generationHead_[slot]];
		auto* recorder = entry.recorder;
		entry.revision = {};
		entry.recorder = nullptr;
		generationHead_[slot] = (generationHead_[slot] + 1) % kPendingGenerationCapacity;
		--generationCount_[slot];
		if (recorder)
			recorder->releaseCommit(false);
	}
}

deluge::loop_station::Error LoopStationView::rollbackGenerations(size_t index, deluge::loop_station::Error failure,
                                                                 uint32_t& logicalSampleFrame, ::Error& recoveryError) {
	auto slot = static_cast<SlotId>(index);
	auto* clip = backingClip(slot);
	logicalSampleFrame = 0;
	recoveryError = ::Error::NONE;
	if (clip) {
		// Snapshot before detaching the working Sample or its live provider. The
		// frame stays stable across storage yields and later tempo changes.
		logicalSampleFrame = currentLogicalSampleFrame(index, *clip);
		// Stop every provisional producer before restoreCurrent(), which may
		// yield while loading the durable revision.
		if (clip->recorder) {
			clip->recorder->captureOutputInputActive = false;
			clip->abortRecording();
		}
		clip->setLoopStationPlaybackRecorder(nullptr);
		clip->unassignVoiceSample(true);
		recoveryError = currentSong->loopStationRevisionHistory[index].restoreCurrent(*clip);
	}
	rollbackFailures_[index] = {.initiating = failure, .recovery = recoveryError};
	discardGenerations(index);
	return failure;
}

void LoopStationView::processPendingCommits() {
	if (!engine_)
		return;
	for (size_t i = 0; i < generationCount_.size(); ++i) {
		auto slot = static_cast<SlotId>(i);
		auto& runtime = engine_->track(slot);
		if (runtime.finishingCapture || !generationCount_[i])
			continue;
		auto& entry = generations_[i][generationHead_[i]];
		auto* recorder = entry.recorder;
		if (!recorder || (recorder->status != RecorderStatus::COMPLETE && recorder->status != RecorderStatus::ABORTED))
			continue;
		auto* clip = backingClip(slot);
		auto& history = currentSong->loopStationRevisionHistory[i];
		bool success = clip && recorder->status == RecorderStatus::COMPLETE && recorder->terminalError == ::Error::NONE
		               && !recorder->hadCardError && !recorder->reachedMaxFileSize;
		auto error = recorder->terminalError != ::Error::NONE ? recorderError(recorder->terminalError)
		             : recorder->captureOutputBackingMissing  ? deluge::loop_station::Error::BACKING_UNAVAILABLE
		             : recorder->reachedMaxFileSize           ? deluge::loop_station::Error::MAX_FILE_SIZE
		                                                      : deluge::loop_station::Error::SD_CARD;
		uint32_t logicalSampleFrame = clip ? currentLogicalSampleFrame(i, *clip) : 0;
		uint32_t rollbackLogicalSampleFrame = logicalSampleFrame;
		::Error recoveryError = ::Error::NONE;
		deluge::loop_station::resolveGenerationCommit(
		    runtime.finishingCapture, success,
		    [&] {
			    if (!history.publishGeneration(std::move(entry.revision))) {
				    error = deluge::loop_station::Error::ALLOCATION;
				    return false;
			    }
			    entry.recorder = nullptr;
			    entry.revision = {};
			    generationHead_[i] = (generationHead_[i] + 1) % kPendingGenerationCapacity;
			    --generationCount_[i];
			    pendingCommitRecorders_[i] =
			        generationCount_[i] ? generations_[i][generationHead_[i]].recorder : nullptr;
			    recorder->releaseCommit(true);
			    return true;
		    },
		    [&] { error = rollbackGenerations(i, error, rollbackLogicalSampleFrame, recoveryError); },
		    [&](bool published) {
			    engine_->notifyCommitComplete(slot, published, error);
			    if (published && clip && clip->loopStationPlaybackRecorder == recorder && !runtime.stopAfterCapture)
				    resumeCommittedClip(slot, logicalSampleFrame);
			    else if (!published && recoveryError == ::Error::NONE && clip && !runtime.stopAfterCapture
			             && !clip->isEmpty(false))
				    resumeCommittedClip(slot, rollbackLogicalSampleFrame);
		    });
	}
}

void LoopStationView::restartPlayback(SlotId slot) {
	AudioClip* clip = backingClip(slot);
	if (!clip || clip->isEmpty(false) || !playbackHandler.isEitherClockActive())
		return;
	char memory[MODEL_STACK_MAX_SIZE];
	auto* stack = setupModelStackWithTimelineCounter(memory, currentSong, clip);
	clip->expectNoFurtherTicks(currentSong, true);
	uint32_t frames = std::max<uint32_t>(1, clip->sampleHolder.getDurationInSamples(true));
	if (clip->loopStationPlaybackRecorder) {
		clip->loopStationPlaybackRecorder->setLoopPlaybackCursor((frames - clip->loopStationPhaseZeroFrames())
		                                                         % frames);
		static_cast<AudioOutput*>(clip->output)->resetEnvelope();
		return;
	}
	clip->resumeLoopStationAtFrame(stack, (frames - clip->loopStationPhaseZeroFrames()) % frames);
}

void LoopStationView::stateChanged(SlotId slot, TrackState state) {
	uiNeedsRendering(this, 0, 1u << kGroupControlsRow);
	AudioClip* clip = backingClip(slot);
	if (state == TrackState::ERROR) {
		switch (engine_->track(slot).error) {
		case deluge::loop_station::Error::INTERNAL:
			display->displayError(::Error::BUG);
			break;
		case deluge::loop_station::Error::BACKING_UNAVAILABLE:
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_LOOP_STATION_BACKING_UNAVAILABLE));
			break;
		case deluge::loop_station::Error::MAX_FILE_SIZE:
			display->displayError(::Error::MAX_FILE_SIZE_REACHED);
			break;
		case deluge::loop_station::Error::SD_CARD:
			display->displayError(::Error::SD_CARD);
			break;
		case deluge::loop_station::Error::SD_CARD_FULL:
			display->displayError(::Error::SD_CARD_FULL);
			break;
		case deluge::loop_station::Error::SD_CARD_NOT_PRESENT:
			display->displayError(::Error::SD_CARD_NOT_PRESENT);
			break;
		case deluge::loop_station::Error::SD_CARD_NO_FILESYSTEM:
			display->displayError(::Error::SD_CARD_NO_FILESYSTEM);
			break;
		case deluge::loop_station::Error::WRITE_PROTECTED:
			display->displayError(::Error::WRITE_PROTECTED);
			break;
		case deluge::loop_station::Error::RESOURCE_LIMIT:
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_LOOP_STATION_RECORDER_LIMIT));
			break;
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
		if (needsResume && !clip->loopStationPlaybackRecorder && playbackHandler.isEitherClockActive()
		    && !clip->isEmpty(false)) {
			uint32_t frames = std::max<uint32_t>(1, clip->sampleHolder.getDurationInSamples(true));
			clip->resumeLoopStationAtFrame(modelStack, (frames - clip->loopStationPhaseZeroFrames()) % frames);
		}
		refreshMonitoring();
		updateUndoLed();
		return;
	}
	if (state == TrackState::RECORDING || state == TrackState::OVERDUBBING) {
		bool needsPhraseResume = state == TrackState::OVERDUBBING && !clip->voiceSample
		                         && !clip->loopStationPlaybackRecorder && !clip->isEmpty(false);
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
			    static_cast<int32_t>(samplePhaseForLoopPhase(logicalPhase, clip->loopStationPhaseZeroTicks(), length)),
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

uint64_t LoopStationView::playbackPositionQ16(SlotId slot) {
	AudioClip* clip = backingClip(slot);
	if (!clip || (!clip->voiceSample && !clip->loopStationPlaybackRecorder) || !clip->sampleHolder.audioFile
	    || clip->loopLength <= 0) {
		return 0;
	}
	auto* sample = static_cast<Sample*>(clip->sampleHolder.audioFile);
	uint32_t duration = std::max<uint32_t>(1, clip->sampleHolder.getDurationInSamples(true));
	int64_t relative = clip->loopStationPlaybackRecorder
	                       ? clip->loopStationPlaybackRecorder->loopPlaybackCursor()
	                       : static_cast<int64_t>(clip->voiceSample->getPlaySample(sample, &clip->guide))
	                             - clip->sampleHolder.startPos;
	if (relative < 0) {
		relative = 0;
	}
	uint32_t samplePhase = static_cast<uint32_t>(static_cast<uint64_t>(relative) % duration);
	uint32_t zero = clip->recorder && clip->recorder == clip->loopStationPlaybackRecorder
	                    ? overdubStartFrames_[static_cast<size_t>(slot)]
	                    : clip->loopStationPhaseZeroFrames();
	return (static_cast<uint64_t>((samplePhase + zero) % duration) * 65536 / duration) * clip->loopLength;
}

void LoopStationView::applyTrackSource(SlotId slot) {
	ensureBackingClip(slot);
}

bool LoopStationView::copyTrackConfiguration(SlotId source, SlotId destination) {
	if (!engine_) {
		return false;
	}
	auto const& runtime = engine_->track(destination);
	if (runtime.finishingCapture || runtime.state == TrackState::RECORDING || runtime.state == TrackState::OVERDUBBING
	    || runtime.committingLength || runtime.finishingOverdub) {
		return false;
	}
	if (!deluge::loop_station::copyTrackConfiguration(currentSong->loopStation, source, destination)) {
		return false;
	}
	applyTrackSource(destination);
	refreshMonitoring();
	uiNeedsRendering(this, 0xFFFFFFFF, 0);
	return true;
}

void LoopStationView::refreshMonitoring() {
	std::array<AudioOutput*, kTrackCount> outputs{};
	for (size_t slotIndex = 0; slotIndex < kTrackCount; ++slotIndex) {
		for (int32_t i = 0; i < currentSong->sessionClips.getNumElements(); ++i) {
			Clip* clip = currentSong->sessionClips.getClipAtIndex(i);
			if (clip->type == ClipType::AUDIO && static_cast<AudioClip*>(clip)->loopStationSlotId == slotIndex + 1) {
				auto* audioClip = static_cast<AudioClip*>(clip);
				outputs[slotIndex] = static_cast<AudioOutput*>(audioClip->output);
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
	std::array<bool, kTrackCount> enabledOutputs{};
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
			enabledOutputs[slotIndex] = enabled;
		}
	}
	for (size_t i = 0; i < kTrackCount; ++i) {
		if (!outputs[i])
			continue;
		auto const& settings = currentSong->loopStation.tracks[i].settings;
		auto error =
		    outputs[i]->setLoopStationMonitoring(enabledOutputs[i], settings.transpose, settings.transposeCents);
		if (error != ::Error::NONE)
			display->displayError(error);
	}
}

void LoopStationView::disableMonitoring() {
	// Exit is reached only after pending generations have closed. Hand the
	// audible phase back to normal clip playback before releasing its provider.
	for (size_t i = 0; i < kTrackCount; ++i) {
		auto* clip = backingClip(static_cast<SlotId>(i));
		if (!clip || !clip->loopStationPlaybackRecorder)
			continue;
		auto frame = clip->loopStationPlaybackRecorder->loopPlaybackCursor();
		if (engine_ && engine_->track(static_cast<SlotId>(i)).state == TrackState::PLAYING) {
			char memory[MODEL_STACK_MAX_SIZE];
			auto* stack = setupModelStackWithTimelineCounter(memory, currentSong, clip);
			clip->resumeLoopStationAtFrame(stack, frame);
		}
		clip->setLoopStationPlaybackRecorder(nullptr);
	}
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
	if (x < 0 || x >= kDisplayWidth + kSideBarWidth || y < 0 || y >= kDisplayHeight || !engine_) {
		return ActionResult::DEALT_WITH;
	}
	if (x >= kDisplayWidth) {
		if (velocity && y == kGroupControlsRow) {
			if (x == kStartAllX && engine_->canStartAll()) {
				if (!playbackHandler.isEitherClockActive()) {
					playbackHandler.setupPlaybackUsingInternalClock(0, false, false, true);
				}
				engine_->startAll(transport());
			}
			else if (x == kStopAllX && engine_->canStopAll()) {
				engine_->stopAll(transport());
			}
			refreshMonitoring();
			playbackHandler.expectEvent();
			uiNeedsRendering(this, 0xFFFFFFFF, 0xFFFFFFFF);
		}
		return ActionResult::DEALT_WITH;
	}
	auto slot = Geometry::slotAt(x, y);
	if (engine_->track(slot).finishingCapture)
		return ActionResult::DEALT_WITH;
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
		if (!engine_->track(slot).committingLength && !engine_->track(slot).finishingOverdub) {
			clearBackingClip(slot);
			engine_->clearHeld(slot);
		}
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
	// Loading/clearing a Song can destroy clip/history objects. Drain capture
	// before opening those screens, including when startup yielded to this button.
	if ((button == LOAD || button == SAVE) && (engine_->anyCapturing() || exitRequested_)) {
		if (on) {
			deferredGlobalButton_ = button;
			deferredGlobalButtonReleased_ = false;
			deferredGlobalShift_ = Buttons::isShiftButtonPressed();
			requestExit();
		}
		else if (deferredGlobalButton_ == button) {
			deferredGlobalButtonReleased_ = true;
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
		completeExit();
	}
	else {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_LOOP_STATION_COMMITTING));
	}
}

void LoopStationView::completeExit() {
	auto deferred = deferredGlobalButton_;
	bool released = deferredGlobalButtonReleased_;
	bool shifted = deferredGlobalShift_;
	deferredGlobalButton_.reset();
	deferredGlobalButtonReleased_ = false;
	deferredGlobalShift_ = false;
	exitRequested_ = false;
	disableMonitoring();
	changeRootUI(&sessionView);
	if (deferred) {
		if (shifted) {
			if (*deferred == deluge::hid::button::LOAD
			    && deluge::gui::context_menu::clearSong.setupAndCheckAvailability())
				openUI(&deluge::gui::context_menu::clearSong);
		}
		else {
			view.buttonAction(*deferred, true, false);
			if (released)
				view.buttonAction(*deferred, false, false);
		}
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
	case PadColour::GOLD:
		return yellow_orange;
	case PadColour::LIME:
		return lime;
	case PadColour::TURQUOISE:
		return turquoise;
	case PadColour::SKY:
		return darkblue;
	case PadColour::PINK:
		return pink_full;
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
			case TrackState::ERROR:
				centre = rapidBlink ? deluge::gui::colours::red : black;
				border = deluge::gui::colours::red.dim(3);
				break;
			}
		}

		auto progressFill = Geometry::progressFill(playbackPositionQ16(slot), runtime.phraseLength);

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
		if (whichRows & (1u << kGroupControlsRow)) {
			image[kGroupControlsRow][kStartAllX] =
			    engine_ && engine_->canStartAll() ? deluge::gui::colours::green : deluge::gui::colours::green.dim(3);
			image[kGroupControlsRow][kStopAllX] =
			    engine_ && engine_->canStopAll() ? deluge::gui::colours::red : deluge::gui::colours::red.dim(3);
			bool startPending = false, stopPending = false;
			if (engine_)
				for (size_t i = 0; i < kTrackCount; ++i) {
					auto action = engine_->track(static_cast<SlotId>(i)).event.action;
					startPending |= action == PendingAction::START_RECORDING || action == PendingAction::RETRIGGER;
					stopPending |= action == PendingAction::STOP || action == PendingAction::FINISH_RECORDING;
				}
			bool lit = (AudioEngine::audioSampleTimer / (kSampleRate / 8) & 1) == 0;
			if (startPending)
				image[kGroupControlsRow][kStartAllX] = lit ? deluge::gui::colours::green : black;
			if (stopPending)
				image[kGroupControlsRow][kStopAllX] = lit ? deluge::gui::colours::red : black;
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
	serviceBackgroundWork();
	indicator_leds::setLedState(IndicatorLED::RECORD, engine_->anyCapturing());
	uiNeedsRendering(this, 0xFFFFFFFF, 1u << kGroupControlsRow);
	if (exitRequested_ && engine_->exitReady()) {
		completeExit();
	}
}

void LoopStationView::processMusicalTick() {
	// Clip sequencing still runs on swung ticks. User actions run below on the
	// audio clock, including fractional intervals between those ticks.
}

void LoopStationView::finalizeAudioWindow(size_t& numSamples) {
	if (!engine_ || !playbackHandler.isEitherClockActive())
		return;
	auto now = transport();
	lastAudioPositionQ16_ = (static_cast<uint64_t>(now.tick) << 16) | now.fractionQ16;
	engine_->processTick(now);
	uint64_t next = engine_->nextActionPositionQ16();
	if (next != UINT64_MAX && next > lastAudioPositionQ16_) {
		uint64_t ticks = next - lastAudioPositionQ16_;
		uint64_t samplesPerTick = std::max<uint32_t>(1, playbackHandler.getTimePerInternalTick());
		// Only multiply when the deadline can lie in this window (also bounds
		// arithmetic for very long pending durations).
		if (ticks <= ((static_cast<uint64_t>(numSamples) << 16) / samplesPerTick) + 1) {
			numSamples = std::min<uint64_t>(numSamples, std::max<uint64_t>(1, (ticks * samplesPerTick + 65535) >> 16));
		}
	}
}

void LoopStationView::serviceBackgroundWork() {
	if (!engine_)
		return;
	processPendingCommits();
	engine_->serviceAudioCompletion(transport());
	for (size_t i = 0; i < kTrackCount; ++i) {
		auto slot = static_cast<SlotId>(i);
		auto const& runtime = engine_->track(slot);
		AudioClip* clip = backingClip(slot);
		auto* recorder = clip ? (clip->recorder ? clip->recorder : clip->loopStationPlaybackRecorder) : nullptr;
		if (runtime.finishingCapture
		    || (runtime.state != TrackState::RECORDING && runtime.state != TrackState::OVERDUBBING
		        && !runtime.finishingOverdub && !(runtime.state == TrackState::PLAYING && recorder))) {
			continue;
		}
		deluge::loop_station::Error failure = deluge::loop_station::Error::NONE;
		if (!recorder)
			failure = deluge::loop_station::Error::ALLOCATION;
		else if (recorder->reachedMaxFileSize)
			failure = deluge::loop_station::Error::MAX_FILE_SIZE;
		else if (recorder->terminalError != ::Error::NONE)
			failure = recorderError(recorder->terminalError);
		else if (recorder->hadCardError)
			failure = deluge::loop_station::Error::SD_CARD;
		else if (recorder->captureOutputBackingMissing) {
			failure = deluge::loop_station::Error::BACKING_UNAVAILABLE;
		}
		else if (recorder->status == RecorderStatus::ABORTED) {
			failure = deluge::loop_station::Error::ALLOCATION;
		}
		if (failure != deluge::loop_station::Error::NONE) {
			bool notifiedCommitFailure = runtime.pendingCommits != 0;
			uint32_t rollbackLogicalSampleFrame = clip ? currentLogicalSampleFrame(i, *clip) : 0;
			::Error recoveryError = ::Error::NONE;
			deluge::loop_station::resolveGenerationCommit(
			    engine_->track(slot).finishingCapture, false, [] { return false; },
			    [&] {
				    if (clip && (clip->recorder || clip->loopStationPlaybackRecorder))
					    failure = rollbackGenerations(i, failure, rollbackLogicalSampleFrame, recoveryError);
			    },
			    [&](bool) {
				    if (notifiedCommitFailure)
					    engine_->notifyCommitComplete(slot, false, failure);
				    else
					    engine_->notifyFailure(slot, failure);
				    if (clip && recoveryError == ::Error::NONE && !runtime.stopAfterCapture && !clip->isEmpty(false))
					    resumeCommittedClip(slot, rollbackLogicalSampleFrame);
			    });
		}
	}
	updateRecordingMode();
	if (monitoringCaptureMask_ != capturingMask()) {
		refreshMonitoring();
	}
	// A completed commit can create an auto-overdub or another deadline. Ask
	// the playback handler to wake/reschedule safely; graphics never executes
	// queued musical actions or directly mutates its relative tick interval.
	if (engine_->anyCapturing()
	    || engine_->ticksUntilNextEvent(static_cast<uint32_t>(playbackHandler.lastSwungTickActioned))
	           != std::numeric_limits<uint32_t>::max()) {
		playbackHandler.expectEvent();
	}
}

void LoopStationView::playbackEnded() {
	if (engine_) {
		auto now = transport();
		auto position = std::max((uint64_t(now.tick) << 16) | now.fractionQ16, lastAudioPositionQ16_);
		now.tick = position >> 16;
		now.fractionQ16 = position & 0xFFFF;
		engine_->transportStopped(now);
		updateRecordingMode();
	}
	uiNeedsRendering(this, 0xFFFFFFFF, 0);
}
