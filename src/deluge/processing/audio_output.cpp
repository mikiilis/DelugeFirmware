/*
 * Copyright © 2019-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "processing/audio_output.h"
#include "definitions.h"
#include "definitions_cxx.hpp"
#include "dsp/stereo_sample.h"
#include "gui/views/view.h"
#include "loop_station/loop_station.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/audio_clip.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "modulation/params/param_manager.h"
#include "playback/mode/arrangement.h"
#include "playback/mode/playback_mode.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/live/live_pitch_shifter.h"
#include "storage/audio/audio_file.h"
#include "storage/storage_manager.h"
#include "util/functions.h"
#include "util/lookuptables/lookuptables.h"
#include <algorithm>
#include <array>
#include <new>
#include <ranges>

extern "C" {
#include "drivers/ssi/ssi.h"
}

namespace params = deluge::modulation::params;
EnumStringMap<AudioOutputMode, kNumAudioOutputModes> aoModeStringMap({{
    {AudioOutputMode::player, "Player"},
    {AudioOutputMode::sampler, "Sampler"},
    {AudioOutputMode::looper, "Looper/FX"},
}});
AudioOutputMode stringToAOMode(char const* string) {
	return aoModeStringMap(string);
}
char const* aoModeToString(AudioOutputMode mode) {
	return aoModeStringMap(mode);
}

namespace {
bool isPhysicalInput(AudioInputChannel channel) {
	return channel >= AudioInputChannel::LEFT && channel < AUDIO_INPUT_CHANNEL_FIRST_INTERNAL_OPTION;
}

void addPhysicalInput(std::span<StereoSample> output, AudioInputChannel requestedChannel, int32_t amplitudeAtStart,
                      int32_t amplitudeAtEnd, uint8_t gainShift, bool saturate = false) {
	int32_t const* inputPtr = reinterpret_cast<int32_t const*>(AudioEngine::i2sRXBufferPos);
	AudioInputChannel channel = requestedChannel;
	if (channel == AudioInputChannel::STEREO && !AudioEngine::renderInStereo) {
		channel = AudioInputChannel::NONE; // Combine both inputs when the render path is mono.
	}

	auto amplitudeIncrement = static_cast<int32_t>(static_cast<double>(amplitudeAtEnd - amplitudeAtStart)
	                                               / static_cast<double>(output.size()));
	int32_t amplitude = amplitudeAtStart;
	for (StereoSample& outputSample : output) {
		amplitude += amplitudeIncrement;
		StereoSample input = {
		    .l = multiply_32x32_rshift32(inputPtr[0], amplitude) << gainShift,
		    .r = multiply_32x32_rshift32(inputPtr[1], amplitude) << gainShift,
		};

		auto add = [&outputSample, saturate](StereoSample value) {
			if (saturate) {
				outputSample.l = deluge::loop_station::saturatingAdd(outputSample.l, value.l);
				outputSample.r = deluge::loop_station::saturatingAdd(outputSample.r, value.r);
			}
			else {
				outputSample += value;
			}
		};
		switch (channel) {
		case AudioInputChannel::LEFT:
			add(StereoSample::fromMono(input.l));
			break;
		case AudioInputChannel::RIGHT:
			add(StereoSample::fromMono(input.r));
			break;
		case AudioInputChannel::BALANCED:
			add(StereoSample::fromMono((input.l / 2) - (input.r / 2)));
			break;
		case AudioInputChannel::NONE:
			add(StereoSample::fromMono((input.l / 2) + (input.r / 2)));
			break;
		default: // STEREO
			add(input);
			break;
		}

		inputPtr += NUM_MONO_INPUT_CHANNELS;
		if (inputPtr >= getRxBufferEnd()) {
			inputPtr -= SSI_RX_BUFFER_NUM_SAMPLES * NUM_MONO_INPUT_CHANNELS;
		}
	}
}

constexpr float semitonesToRatio(int16_t semitones, int8_t cents) {
	float totalSemitones = static_cast<float>(semitones) + static_cast<float>(cents) / 100.0f;
	return std::pow(2.0f, totalSemitones / 12.0f);
}

// Convert Loop Station transpose (semitones * 100 + cents) to phaseIncrement for LivePitchShifter
// PhaseIncrement format: kMaxSampleValue = 1.0 (unity), > kMaxSampleValue = pitch up, < kMaxSampleValue = pitch down
// The adjustment parameter for getExp uses 26 bits per octave, so 1 semitone = (1<<26)/12
static constexpr int32_t kCentsToAdjustment = (1 << 26) / 1200; // ~55924 per cent

int32_t transposeToPhaseIncrement(int16_t semitones, int8_t cents) {
	int32_t totalCents = semitones * 100 + cents;
	if (totalCents == 0) {
		return kMaxSampleValue;
	}
	int32_t adjustment = totalCents * kCentsToAdjustment;
	return getExp(kMaxSampleValue, adjustment);
}

OscType audioInputChannelToOscType(AudioInputChannel channel) {
	switch (channel) {
	case AudioInputChannel::LEFT:
		return OscType::INPUT_L;
	case AudioInputChannel::RIGHT:
		return OscType::INPUT_R;
	case AudioInputChannel::STEREO:
		return OscType::INPUT_STEREO;
	case AudioInputChannel::BALANCED:
	case AudioInputChannel::NONE:
		return OscType::INPUT_L; // Mix to mono left
	default:
		return OscType::INPUT_L;
	}
}
} // namespace

void AudioOutput::updateLoopStationPitchShifter() {
	// Determine if we need a pitch shifter
	bool needPitchShifter = loopStationMonitoring && (loopStationTranspose != 0 || loopStationTransposeCents != 0);

	// Calculate required pitch ratio
	int32_t newPhaseIncrement = kMaxSampleValue;
	OscType newInputType = OscType::INPUT_L;

	if (needPitchShifter) {
		newPhaseIncrement = transposeToPhaseIncrement(loopStationTranspose, loopStationTransposeCents);
		newInputType = audioInputChannelToOscType(inputChannel);

		// Handle stereo fallback like voice.cpp does
		if (newInputType == OscType::INPUT_STEREO && !AudioEngine::lineInPluggedIn && !AudioEngine::micPluggedIn) {
			newInputType = OscType::INPUT_L;
		}
	}

	// Check if we need to recreate the pitch shifter
	bool recreate = false;
	if (needPitchShifter) {
		if (!loopStationPitchShifter) {
			recreate = true; // Create new
		}
		else if (loopStationPitchPhaseIncrement != newPhaseIncrement || loopStationPitchInputType != newInputType) {
			recreate = true; // Settings changed
		}
	}
	else {
		recreate = false; // Will destroy if exists
	}

	if (recreate) {
		// Destroy old one if exists
		if (loopStationPitchShifter) {
			loopStationPitchShifter->~LivePitchShifter();
			delugeDealloc(loopStationPitchShifter);
			loopStationPitchShifter = nullptr;
		}

		// Create new one
		if (needPitchShifter) {
			// Ensure LiveInputBuffer exists before creating pitch shifter (like voice.cpp does)
			AudioEngine::getOrCreateLiveInputBuffer(newInputType, true);

			void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(LivePitchShifter));
			if (memory) {
				loopStationPitchShifter = new (memory) LivePitchShifter(newInputType, newPhaseIncrement);
				loopStationPitchPhaseIncrement = newPhaseIncrement;
				loopStationPitchInputType = newInputType;
			}
		}
	}
	else if (!needPitchShifter && loopStationPitchShifter) {
		// No longer needed - destroy
		loopStationPitchShifter->~LivePitchShifter();
		delugeDealloc(loopStationPitchShifter);
		loopStationPitchShifter = nullptr;
	}
}

void AudioOutput::addPhysicalInputWithPitchShift(std::span<StereoSample> output, AudioInputChannel requestedChannel,
                                                 int32_t amplitudeAtStart, int32_t amplitudeAtEnd, uint8_t gainShift,
                                                 bool saturate, float pitchRatio) {
	// If no pitch shifting needed, use normal path
	if (pitchRatio == 1.0f || !loopStationPitchShifter) {
		addPhysicalInput(output, requestedChannel, amplitudeAtStart, amplitudeAtEnd, gainShift, saturate);
		return;
	}

	// Use LivePitchShifter for real-time pitch shifting
	// The LivePitchShifter internally manages the input buffer and renders pitch-shifted output
	// We need to render to a temporary buffer then mix into output with proper channel mapping

	int32_t numChannels = loopStationPitchShifter->numChannels;
	int32_t numSamples = static_cast<int32_t>(output.size());

	// Allocate temporary buffer for pitch-shifted output
	// Size: numSamples * numChannels (interleaved)
	alignas(CACHE_LINE_SIZE) std::array<int32_t, SSI_TX_BUFFER_NUM_SAMPLES * 2> tempBuffer;
	std::span<int32_t> tempSpan{tempBuffer.data(),
	                            static_cast<uint32_t>(numSamples) * static_cast<uint32_t>(numChannels)};

	// Clear temp buffer to avoid artifacts from partial fills (pitch shifter may not fill all samples)
	std::ranges::fill(tempSpan, 0);

	// Get interpolation buffer size for this pitch ratio
	// For live input, use a reasonable default (similar to voice.cpp)
	int32_t interpolationBufferSize = 0; // Use default

	// Calculate amplitude increment per sample
	// Pass amplitude directly without gainShift - pitch shifter handles its own scaling internally
	int32_t amplitudeIncrement =
	    static_cast<int32_t>(static_cast<double>(amplitudeAtEnd - amplitudeAtStart) / static_cast<double>(numSamples));

	// Scale amplitude to match what LivePitchShifter expects
	// In voice.cpp, sourceAmplitude max is ~134217727 (0x7FFFFFF)
	// Our envelope outputs ~1073741824 to 2147483647, which is ~8x larger
	// Pitch shifter then applies << 5 internally
	// Scale down by 8 (>> 3) to match voice.cpp amplitude range
	int32_t scaledAmplitudeStart = amplitudeAtStart >> 3;
	int32_t scaledAmplitudeIncrement = amplitudeIncrement >> 3;

	// Render pitch-shifted audio to temp buffer
	// The LivePitchShifter expects interleaved output with numChannels channels
	loopStationPitchShifter->render(tempSpan.data(), numSamples, loopStationPitchPhaseIncrement,
	                                scaledAmplitudeStart, // Scaled down to match voice.cpp amplitude range
	                                scaledAmplitudeIncrement, interpolationBufferSize);

	// Now mix the pitch-shifted audio into the output buffer with proper channel handling
	if (numChannels == 1) {
		// Mono input - expand to stereo
		for (int32_t i = 0; i < numSamples; ++i) {
			int32_t sample = tempSpan[i];
			StereoSample& outputSample = output[i];
			if (saturate) {
				outputSample.l = deluge::loop_station::saturatingAdd(outputSample.l, sample);
				outputSample.r = deluge::loop_station::saturatingAdd(outputSample.r, sample);
			}
			else {
				outputSample.l += sample;
				outputSample.r += sample;
			}
		}
	}
	else {
		// Stereo input - interleaved L,R,L,R...
		for (int32_t i = 0; i < numSamples; ++i) {
			StereoSample pitchShiftedSample;
			pitchShiftedSample.l = tempSpan[i * 2];
			pitchShiftedSample.r = tempSpan[i * 2 + 1];

			StereoSample& outputSample = output[i];
			if (saturate) {
				outputSample.l = deluge::loop_station::saturatingAdd(outputSample.l, pitchShiftedSample.l);
				outputSample.r = deluge::loop_station::saturatingAdd(outputSample.r, pitchShiftedSample.r);
			}
			else {
				outputSample += pitchShiftedSample;
			}
		}
	}
}

AudioOutput::AudioOutput() : Output(OutputType::AUDIO) {
	modKnobMode = 0;
	inputChannel = AudioInputChannel::LEFT;
	mode = AudioOutputMode::player;
}

AudioOutput::~AudioOutput() {
	releaseMonitoringClaim();
	// Clean up pitch shifter
	if (loopStationPitchShifter) {
		loopStationPitchShifter->~LivePitchShifter();
		delugeDealloc(loopStationPitchShifter);
		loopStationPitchShifter = nullptr;
	}
}

void AudioOutput::setLoopStationMonitoring(bool enabled, int16_t transpose, int8_t transposeCents) {
	// Reset pitch shifter when enabling or when transpose changes
	if (enabled
	    && (!loopStationMonitoring || loopStationTranspose != transpose
	        || loopStationTransposeCents != transposeCents)) {
		// Will be recreated in updateLoopStationPitchShifter
	}
	loopStationMonitoring = enabled;
	loopStationTranspose = transpose;
	loopStationTransposeCents = transposeCents;
	updateLoopStationPitchShifter();
}

void AudioOutput::scrollAudioOutputMode(int offset) {
	auto modeInt = util::to_underlying(mode);
	modeInt = (modeInt + offset) % kNumAudioOutputModes;
	auto requested = static_cast<AudioOutputMode>(std::clamp<int>(modeInt, 0, kNumAudioOutputModes - 1));
	Clip* clip = getActiveClip();
	bool loopStationBound =
	    clip && clip->type == ClipType::AUDIO && static_cast<AudioClip*>(clip)->loopStationSlotId != 0;
	if (loopStationBound && requested == AudioOutputMode::looper) {
		display->displayPopup("LOOP SLOT");
		return;
	}
	mode = requested;
	updateMonitoringClaim();
	renderUIsForOled();
	if (display->have7SEG()) {
		const char* type;
		switch (mode) {
		case AudioOutputMode::player:
			type = "PLAY";
			break;
		case AudioOutputMode::sampler:
			type = "SAMP";
			break;
		case AudioOutputMode::looper:
			type = "LOOP";
			break;
		}
		display->displayPopup(type);
	}
}

void AudioOutput::cloneFrom(ModControllableAudio* other) {
	GlobalEffectableForClip::cloneFrom(other);
	auto ao = ((AudioOutput*)other);
	inputChannel = ao->inputChannel;

	auto modeInt = util::to_underlying(ao->mode);

	mode = static_cast<AudioOutputMode>(std::clamp<int>(modeInt, 0, kNumAudioOutputModes - 1));
	outputRecordingFrom = nullptr;
	// old style cloning overdubs
	if (mode == AudioOutputMode::looper || mode == AudioOutputMode::sampler) {
		// if the original track hasn't been recorded into then we'll just be a player. Avoids doubling monitoring
		if (ao->isEmpty()) {
			mode = AudioOutputMode::player;
		}
		else {
			// otherwise we'll become the new sampler/looper and the og will become a player
			ao->mode = AudioOutputMode::player;
		}
	}

	if (inputChannel == AudioInputChannel::SPECIFIC_OUTPUT) {
		outputRecordingFrom = ao->outputRecordingFrom;
		// now steal the monitoring of the original track if necessary (i.e. we're a looper or sampler)
		updateMonitoringClaim();
	}
}

void AudioOutput::renderOutput(ModelStack* modelStack, std::span<StereoSample> output, int32_t* reverbBuffer,
                               int32_t reverbAmountAdjust, int32_t sideChainHitPending, bool shouldLimitDelayFeedback,
                               bool isClipActive) {

	ParamManager* paramManager = getParamManager(modelStack->song);

	ModelStackWithTimelineCounter* modelStackWithTimelineCounter = modelStack->addTimelineCounter(activeClip);

	GlobalEffectableForClip::renderOutput(modelStackWithTimelineCounter, paramManager, output, reverbBuffer,
	                                      reverbAmountAdjust, sideChainHitPending, shouldLimitDelayFeedback,
	                                      isClipActive, OutputType::AUDIO, recorder);
}

void AudioOutput::resetEnvelope() {
	if (activeClip) {
		AudioClip* activeAudioClip = (AudioClip*)activeClip;

		bool directlyToDecay = (activeAudioClip->attack == -2147483648);
		envelope.noteOn(directlyToDecay, 2147483647);
	}
	amplitudeLastTime = 0;
	overrideAmplitudeEnvelopeReleaseRate = 0;
}

bool AudioOutput::modeAllowsMonitoring() const {
	if (loopStationMonitoring && inputChannel >= AudioInputChannel::LEFT
	    && inputChannel < AUDIO_INPUT_CHANNEL_FIRST_INTERNAL_OPTION) {
		return true;
	}
	if (mode == AudioOutputMode::player) {
		return false;
	}
	if (mode == AudioOutputMode::sampler) {
		// Reachable with a null activeClip via the SPECIFIC_OUTPUT branch, which renders a record-source output
		// directly and so bypasses the inValidState guard in Song::renderAudio.
		if (!activeClip) {
			return false;
		}
		auto& activeAudioClip = static_cast<AudioClip&>(*activeClip);
		return activeAudioClip.isEmpty();
	}
	if (mode == AudioOutputMode::looper) {
		return true;
	}
	return false;
}

// Beware - unlike usual, modelStack, a ModelStackWithThreeMainThings*,  might have a NULL timelineCounter
bool AudioOutput::renderGlobalEffectableForClip(ModelStackWithTimelineCounter* modelStack,
                                                std::span<StereoSample> render, int32_t* bufferToTransferTo,
                                                int32_t* reverbBuffer, int32_t reverbAmountAdjust,
                                                int32_t sideChainHitPending, bool shouldLimitDelayFeedback,
                                                bool isClipActive, int32_t pitchAdjust, int32_t amplitudeAtStart,
                                                int32_t amplitudeAtEnd) {
	bool rendered = false;
	bool capturingPreFXOverdub = recorder && recorder->captureOutputPreFX
	                             && recorder->status < RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING;
	bool backingRenderedForCapture = false;
	// Audio outputs can have an activeClip while being muted. A Loop Station
	// overdub must still render that backing clip into its private capture pass;
	// activity bookkeeping must never turn an Add overdub into input-only Replace.
	if ((isClipActive || capturingPreFXOverdub) && activeClip) {
		auto& activeAudioClip = static_cast<AudioClip&>(*activeClip);
		if (activeAudioClip.voiceSample) {

			int32_t attackNeutralValue = paramNeutralValues[params::LOCAL_ENV_0_ATTACK];
			int32_t attack = getExp(attackNeutralValue, -(activeAudioClip.attack / 4));

renderEnvelope:
			int32_t amplitudeLocal =
			    (envelope.render(render.size(), attack, 8388608, 2147483647, 0, decayTableSmall4) >> 1) + 1073741824;

			if (envelope.state >= EnvelopeStage::OFF) {
				if (activeAudioClip.doingLateStart) {
					resetEnvelope();
					goto renderEnvelope;
				}

				else {
					// I think we can only be here for one shot audio clips, so maybe we shouldn't keep it?
					activeAudioClip.unassignVoiceSample(false);
				}
			}

			else {

				if (!amplitudeLastTime && attack > 245632) {
					amplitudeLastTime = amplitudeLocal;
				}

				int32_t amplitudeEffectiveStart =
				    multiply_32x32_rshift32(amplitudeLastTime, amplitudeAtStart); // Reduces amplitude another >>1
				int32_t amplitudeEffectiveEnd =
				    multiply_32x32_rshift32(amplitudeLocal, amplitudeAtEnd); // Reduces amplitude another >>1

				int32_t amplitudeIncrementEffective =
				    static_cast<int32_t>(static_cast<double>(amplitudeEffectiveEnd - amplitudeEffectiveStart)
				                         / static_cast<double>(render.size()));

				std::span render_mono{reinterpret_cast<q31_t*>(render.data()), render.size()};
				backingRenderedForCapture = activeAudioClip.render(modelStack, render_mono, amplitudeEffectiveStart,
				                                                   amplitudeIncrementEffective, pitchAdjust);
				rendered = isClipActive;
				amplitudeLastTime = amplitudeLocal;

				// If we need to duplicate mono to stereo...
				if (AudioOutput::willRenderAsOneChannelOnlyWhichWillNeedCopying()) {
					// If can write directly into Song buffer...
					if (bufferToTransferTo != nullptr) {
						std::ranges::transform(render_mono, reinterpret_cast<StereoSample*>(bufferToTransferTo),
						                       StereoSample::fromMono);
					}

					// Or, if duplicating within same rendering buffer (cos there's FX to be applied)
					else {
						std::ranges::transform(render_mono.rbegin(), render_mono.rend(), render.rbegin(),
						                       StereoSample::fromMono);
					}
				}
			}
		}
	}

	std::span<StereoSample> output = (bufferToTransferTo != nullptr)
	                                     ? std::span{reinterpret_cast<StereoSample*>(bufferToTransferTo), render.size()}
	                                     : render;

	// A Loop Station overdub records a dry cumulative pass. Copy the old
	// loop before audible monitoring and track FX, then add the physical input
	// only while Overdub is visibly enabled. This deliberately does not depend on
	// the Monitoring setting: monitoring controls the live return, not recording.
	if (capturingPreFXOverdub) {
		if (!backingRenderedForCapture) {
			// Preserve the previous take. The view checks this latch before asking
			// the view commits its sample and aborts the provisional recorder.
			recorder->captureOutputBackingMissing = true;
		}
		else {
			alignas(CACHE_LINE_SIZE) std::array<StereoSample, SSI_TX_BUFFER_NUM_SAMPLES> captureMemory;
			std::span<StereoSample> capture{captureMemory.data(), output.size()};
			std::ranges::copy(output, capture.begin());
			if (recorder->captureOutputInputActive && isPhysicalInput(inputChannel)) {
				// The Audio Clip dry render has quarter-scale headroom here. Shift the
				// input by three so the recorder's fixed x4 compensation restores both
				// the stored loop and new input to unity.
				float pitchRatio = 1.0f;
				if (loopStationMonitoring && (loopStationTranspose != 0 || loopStationTransposeCents != 0)) {
					pitchRatio = semitonesToRatio(loopStationTranspose, loopStationTransposeCents);
				}
				addPhysicalInputWithPitchShift(capture, inputChannel, amplitudeAtStart, amplitudeAtEnd, 3, true,
				                               pitchRatio);
			}
			recorder->feedAudio(capture, true, 2);
		}
		// During overdub, the existing loop should remain audible.
		// Don't zero the output - the clip has an existing loop that was just rendered.
	}

	// add in the monitored audio if in sampler or looper mode
	if (modeAllowsMonitoring() && modelStack->song->isOutputActiveInArrangement(this)
	    && inputChannel != AudioInputChannel::SPECIFIC_OUTPUT) {
		rendered = true;
		// The audible monitor path retains its normal 6dB of mix headroom.
		float pitchRatio = 1.0f;
		if (loopStationMonitoring && (loopStationTranspose != 0 || loopStationTransposeCents != 0)) {
			pitchRatio = semitonesToRatio(loopStationTranspose, loopStationTransposeCents);
		}
		addPhysicalInputWithPitchShift(output, inputChannel, amplitudeAtStart, amplitudeAtEnd, 2, false, pitchRatio);
	}
	// Only render the record-source if it has an active clip: Output::renderOutput is documented to be called only
	// when there's an activeClip, but this branch bypasses the inValidState guard in Song::renderAudio.
	else if (modeAllowsMonitoring() && modelStack->song->isOutputActiveInArrangement(this)
	         && inputChannel == AudioInputChannel::SPECIFIC_OUTPUT && (outputRecordingFrom != nullptr)
	         && (outputRecordingFrom->getActiveClip() != nullptr)) {
		rendered = true;
		std::byte modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStack* songModelStack = setupModelStackWithSong(modelStackMemory, currentSong);
		outputRecordingFrom->renderOutput(songModelStack, output, reverbBuffer, reverbAmountAdjust, sideChainHitPending,
		                                  shouldLimitDelayFeedback, isClipActive);
	}
	return rendered;
}

bool AudioOutput::willRenderAsOneChannelOnlyWhichWillNeedCopying() {
	return (activeClip && ((AudioClip*)activeClip)->voiceSample
	        && (((AudioClip*)activeClip)->sampleHolder.audioFile->numChannels == 1 || !AudioEngine::renderInStereo));
}

void AudioOutput::cutAllSound() {
	if (activeClip) {
		((AudioClip*)activeClip)->unassignVoiceSample(false);
		((AudioClip*)activeClip)->abortRecording(); // Needed for when this is being called as part of a song-swap - we
		                                            // can't leave recording happening in such a case.
	}
}

void AudioOutput::getThingWithMostReverb(Sound** soundWithMostReverb,
                                         ParamManagerForTimeline** paramManagerWithMostReverb, Kit** kitWithMostReverb,
                                         int32_t* highestReverbAmountFound) {
}

// Unlike for Instruments, AudioOutputs will only be written as part of a Song, so clipForSavingOutputOnly will always
// be NULL
bool AudioOutput::writeDataToFile(Serializer& writer, Clip* clipForSavingOutputOnly, Song* song) {

	writer.writeAttribute("name", name.get());

	writer.writeAttribute("mode", aoModeToString(mode));

	writer.writeAttribute("inputChannel", inputChannelToString(inputChannel));
	writer.writeAttribute("outputRecordingIndex", currentSong->getOutputIndex(outputRecordingFrom));
	Output::writeDataToFile(writer, clipForSavingOutputOnly, song);

	GlobalEffectableForClip::writeAttributesToFile(writer, clipForSavingOutputOnly == nullptr);

	writer.writeOpeningTagEnd();

	ParamManager* paramManager = nullptr;
	// If no activeClip, that means no Clip has this Instrument, so there should be a backedUpParamManager that we
	// should use / save
	if (!activeClip) {
		paramManager = song->getBackedUpParamManagerPreferablyWithClip(this, NULL);
	}

	GlobalEffectableForClip::writeTagsToFile(writer, paramManager, true);

	return true;
}

// clip will always be NULL and is of no consequence - see note in parent output.h
Error AudioOutput::readFromFile(Deserializer& reader, Song* song, Clip* clip, int32_t readAutomationUpToPos) {
	char const* tagName;

	ParamManagerForTimeline paramManager;

	while (*(tagName = reader.readNextTagOrAttributeName())) {

		if (!strcmp(tagName, "echoingInput")) {
			int echoing = reader.readTagOrAttributeValueInt();
			if (echoing) {
				// loopers behave like old monitored clips
				mode = AudioOutputMode::looper;
			}
			reader.exitTag("echoingInput");
		}
		else if (!strcmp(tagName, "mode")) {
			mode = stringToAOMode(reader.readTagOrAttributeValue());
			reader.exitTag("mode");
		}

		else if (!strcmp(tagName, "inputChannel")) {
			inputChannel = stringToInputChannel(reader.readTagOrAttributeValue());
			reader.exitTag("inputChannel");
		}

		else if (!strcmp(tagName, "outputRecordingIndex")) {
			outputRecordingFromIndex = reader.readTagOrAttributeValueInt();
		}

		else if (Output::readTagFromFile(reader, tagName)) {}

		else {

			Error result = GlobalEffectableForClip::readTagFromFile(reader, tagName, &paramManager, 0, nullptr, song);
			if (result == Error::NONE) {}
			else if (result == Error::RESULT_TAG_UNUSED) {
				reader.exitTag();
			}
			else {
				return result;
			}
		}
	}

	if (paramManager.containsAnyMainParamCollections()) {
		song->backUpParamManager(this, NULL, &paramManager);
	}

	return Error::NONE;
}

void AudioOutput::deleteBackedUpParamManagers(Song* song) {
	song->deleteBackedUpParamManagersForModControllable(this);
}

Clip* AudioOutput::createNewClipForArrangementRecording(ModelStack* modelStack) {

	// Allocate memory for audio clip
	void* clipMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(AudioClip));
	if (!clipMemory) {
		return nullptr;
	}

	AudioClip* newClip = new (clipMemory) AudioClip();
	newClip->setOutput(modelStack->addTimelineCounter(newClip), this);

#if ALPHA_OR_BETA_VERSION
	if (!newClip->paramManager.summaries[0].paramCollection) {
		FREEZE_WITH_ERROR("E422"); // Trying to diversify Leo's E410
	}
#endif

	return newClip;
}

bool AudioOutput::wantsToBeginArrangementRecording() {
	return (inputChannel > AudioInputChannel::NONE && Output::wantsToBeginArrangementRecording());
}

bool AudioOutput::setActiveClip(ModelStackWithTimelineCounter* modelStack, PgmChangeSend maySendMIDIPGMs) {
	if (activeClip
	    && (!modelStack || activeClip != modelStack->getTimelineCounter()
	        || (playbackHandler.playbackState && currentPlaybackMode == &arrangement))) {
		((AudioClip*)activeClip)->unassignVoiceSample(false);
	}
	bool clipChanged = Output::setActiveClip(modelStack, maySendMIDIPGMs);

	if (clipChanged) {
		AudioEngine::mustUpdateReverbParamsBeforeNextRender = true;
	}

	return clipChanged;
}

bool AudioOutput::isSkippingRendering() {
	return !loopStationMonitoring && mode == AudioOutputMode::player
	       && (!activeClip || !((AudioClip*)activeClip)->voiceSample);
}

void AudioOutput::getThingWithMostReverb(Sound** soundWithMostReverb, ParamManager** paramManagerWithMostReverb,
                                         GlobalEffectableForClip** globalEffectableWithMostReverb,
                                         int32_t* highestReverbAmountFound) {
	GlobalEffectableForClip::getThingWithMostReverb(activeClip, soundWithMostReverb, paramManagerWithMostReverb,
	                                                globalEffectableWithMostReverb, highestReverbAmountFound);
}

ModelStackWithAutoParam* AudioOutput::getModelStackWithParam(ModelStackWithTimelineCounter* modelStack, Clip* clip,
                                                             int32_t paramID, params::Kind paramKind, bool affectEntire,
                                                             bool useMenuStack) {
	ModelStackWithAutoParam* modelStackWithParam = nullptr;

	ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
	    modelStack->addOtherTwoThingsButNoNoteRow(toModControllable(), &clip->paramManager);

	if (modelStackWithThreeMainThings) {
		modelStackWithParam = modelStackWithThreeMainThings->getUnpatchedAutoParamFromId(paramID);
	}

	return modelStackWithParam;
}
