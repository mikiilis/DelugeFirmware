/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
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

#pragma once
#include "loop_station/storage.h"

#include "definitions_cxx.hpp"
#include "dsp/envelope_follower/absolute_value.h"
#include "dsp/stereo_sample.h"
#include "fatfs/fatfs.hpp"
#include <array>
#include <cstddef>
#include <gsl/gsl>
#include <optional>

enum class MonitoringAction {
	NONE = 0,
	REMOVE_RIGHT_CHANNEL = 1,
	SUBTRACT_RIGHT_CHANNEL = 2,
};

enum class RecorderStatus {
	CAPTURING_DATA = 0,
	CAPTURING_DATA_WAITING_TO_STOP = 1,
	FINISHED_CAPTURING_BUT_STILL_WRITING = 2,
	COMPLETE = 3,

	// Means RAM error only. SD errors are noted separately and won't affect operation, as long as RAM lasts
	ABORTED = 4,
	AWAITING_DELETION = 5,
};

class Sample;
class Cluster;
class AudioClip;
class Output;
struct RecorderConfig {
	bool neverUseThreshold = false;
};

class SampleRecorder {
public:
	SampleRecorder() = default;
	~SampleRecorder();
	Error setup(int32_t newNumChannels, AudioInputChannel newMode, bool newKeepingReasons,
	            bool shouldRecordExtraMargins, AudioRecordingFolder newFolderID, int32_t buttonPressLatency,
	            Output* outputRecordingFrom, RecorderConfig config = {});
	void setRecordingThreshold(RecorderConfig config);
	void feedAudio(std::span<StereoSample> input, bool applyGain = false, uint8_t gainToApply = 5);
	Error cardRoutine();
	// A private cumulative working WAV. Fixed cache size, unchanged durable base.
	Error setupLoopOverdub(uint32_t frames, Sample* source = nullptr, uint32_t sourceStart = 0,
	                       uint32_t sourceFrame = 0);
	void processLoopOverdub(std::span<StereoSample> backing, std::span<const StereoSample> input,
	                        bool backgroundCopy = false);
	void stopLoopOverdubInput() {
		captureOutputInputActive = false;
		loopEnding_ = true;
	}
	[[nodiscard]] bool loopOverdubReady() const {
		return loopFrames_ && loopEnding_ && numSamplesCaptured >= loopFrames_;
	}
	[[nodiscard]] bool isLoopOverdub() const { return loopFrames_ != 0; }
	[[nodiscard]] uint32_t loopFrameCursor() const { return loopFrameCursor_; }

	void endSyncedRecording(int32_t buttonLatencyForTempolessRecording);
	bool inputLooksDifferential();
	bool inputHasNoRightChannel();
	void removeFromOutput() {
		if (status < RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING) {
			abort();
		}
		outputRecordingFrom = nullptr;
	};
	void abort();
	// A provisional clip/history must roll back before abort cleanup may destroy its Sample.
	void retainForCommit();
	void releaseCommit(bool success);
	[[nodiscard]] bool isCommitPending() const { return commitPending_; }
	enum class LifecycleEvent : uint8_t {
		CREATED,
		RETAINED,
		FINISHED,
		ABORTED,
		RELEASED,
		CLEANED,
		DESTROYED,
		TRIM_CHECK,
		TRIM_REJECTED,
		TRIM_COMPLETE,
		CLEANUP_DEFERRED,
		SOURCE_RETAINED,
		CACHE_UNDERRUN,
		STORAGE_FAILED
	};
	void traceLifecycle(LifecycleEvent event, Cluster const* cluster = nullptr, uint32_t targetFileSize = 0) const;

	SampleRecorder* next = nullptr;

	gsl::owner<Sample*> sample = nullptr;

	int32_t numSamplesToRunBeforeBeginningCapturing;
	uint32_t numSamplesBeenRunning = 0;
	uint32_t numSamplesCaptured = 0;

	uint32_t numSamplesExtraToCaptureAtEndSyncingWise;

	int32_t firstUnwrittenClusterIndex = 0;

	// Put things in valid state so if we get destructed before any recording, it's all ok
	int32_t currentRecordClusterIndex = -1;

	// Note! If this is NULL, that means that currentRecordClusterIndex refers to a cluster that never got created (cos
	// some error or max file size reached)
	Cluster* currentRecordCluster = nullptr;

	uint32_t audioFileNumber;
	AudioRecordingFolder folderID;

	char* writePos;
	char* clusterEndPos;

	// When this gets set, we add the Sample to the master list. This is stored here in addition to in the Sample,
	// so we can delete an aborted file even after the Sample has been detached / destructed.
	// This will be the temp file path if there is one.
	String filePathCreated;

	RecorderStatus status = RecorderStatus::CAPTURING_DATA;
	AudioInputChannel mode = AudioInputChannel::NONE;
#if ALPHA_OR_BETA_VERSION
	uint8_t diagnosticLoopStationSlot = 0xFF; // Zero-based; 0xFF means another recorder client.
#endif
	Output* outputRecordingFrom = nullptr; // for when recording from a specific output

	// Need to keep track of this, so we know whether to remove it. Well I guess we could just look and see if it's
	// there... but this is nice.
	bool haveAddedSampleToArray = false;

	bool allowFileAlterationAfter = false;
	bool allowNormalization = true;
	// Loop Station output overdubs capture the dry Audio Clip plus monitored
	// input before track FX / level / pan. Other output recorders retain the
	// legacy post-FX tap.
	bool captureOutputPreFX = false;
	// Independent of audible monitoring: true only while the performer is
	// actively adding input to a Loop Station overdub. It is cleared while the
	// recorder copies the untouched remainder of the fixed-length phrase.
	bool captureOutputInputActive = false;
	// A replacement overdub is never allowed to commit if any capture buffer
	// omitted the previous phrase. Input-only or zero-filled data would otherwise
	// destructively replace the backing WAV.
	bool captureOutputBackingMissing = false;
	bool autoDeleteWhenDone = false;
	bool keepingReasonsForFirstClusters = false;
	uint8_t recordingNumChannels = 0;
	// Preserve the first terminal failure so asynchronous owners can report the
	// real cause after the recorder has detached from its AudioClip. hadCardError
	// remains for compatibility with existing recorder callers.
	deluge::loop_station::StorageFailure storageFailure{};
	uint32_t inputPeak = 0;
	uint32_t capturedPeak = 0;
	Error terminalError = Error::NONE;
	bool hadCardError = false;
	// Loop Station owns presentation of errors for its asynchronous commits.
	// Other recorder users retain the existing global error popup behaviour.
	bool suppressErrorPopup = false;
	bool reachedMaxFileSize = false;
	bool recordingExtraMargins = false;
	bool pointerHeldElsewhere = false;
	bool capturedTooMuch = false;
	bool thresholdRecording = false;
	uint32_t minThresholdMargin = 0;

	// Most of these are not captured in the case of BALANCED input for AudioClips
	bool recordingClippedRecently;
	int32_t recordPeakL;
	int32_t recordPeakR;
	int32_t recordPeakLMinusR;
	uint64_t recordSumL;
	uint64_t recordSumR;
	uint64_t recordSumLPlusR;  // L and R are halved before these two are calculated
	uint64_t recordSumLMinusR; // --------

	int32_t recordMax;
	int32_t recordMin;

	uint32_t audioDataLengthBytesAsWrittenToFile;
	uint32_t loopEndSampleAsWrittenToFile;

	float startValueThreshold;

	int32_t* sourcePos;

	std::optional<FatFS::File> file;

private:
#ifdef IN_UNIT_TESTS
	friend struct SampleRecorderLifecycleTestAccess;
#endif
	struct LoopCacheEntry {
		Cluster* cluster = nullptr;
		uint32_t index = 0;
		bool dirty = false;
		bool writing = false;
	};
	std::array<LoopCacheEntry, 8> loopCache_{};
	Sample* loopSource_ = nullptr;
	uint32_t loopSourceStart_ = 0;
	uint32_t loopSourceFrame_ = 0;
	uint32_t loopFrames_ = 0;
	uint32_t loopFrameCursor_ = 0;
	uint32_t loopPlaybackCursor_ = 0;
	bool loopEnding_ = false;
	bool loopIOActive_ = false;
	bool loopPreparing_ = false;
	void* loopWriteSnapshot_ = nullptr;
	Error prepareLoopClusters();
	Error copyLoopRemainder();
	Error serviceLoopWrites();
	void releaseLoopClusters();
	LoopCacheEntry* loopCacheAt(uint32_t index);
	bool commitPending_ = false;
	bool writerResourcesReleased_ = false;
	bool cleanupActive_ = false;
	bool cleanupDeferred_ = false;
	uint8_t loopNextWriteEntry_ = 0;
	void releaseWriterResources();
	Error latchStorageFailure(deluge::loop_station::StorageFailure failure);
	// Written startup clusters retain their original writer reason for immediate playback.
	// Track the actual ownership, independently of array length (which trimming can change).
	uint8_t retainedStartupClusters_ = 0;
	void releaseRetainedStartupClusters(uint32_t firstIndex = 0);
	Error initializeRecordingSample();
	Error cleanupAbortedRecording();
	void setExtraBytesOnPreviousCluster(Cluster* currentCluster, int32_t currentClusterIndex);
	Error writeCluster(int32_t clusterIndex, size_t numBytes);
	Error alterFile(MonitoringAction action, int32_t lshiftAmount, uint32_t idealFileSizeBeforeAction,

	                uint64_t dataLengthAfterAction);
	Error finalizeRecordedFile();
	Error createNextCluster();
	Error writeAnyCompletedClusters();
	void finishCapturing();
	void updateDataLengthInFirstCluster(Cluster* cluster);
	void totalSampleLengthNowKnown(uint32_t totalLength, uint32_t loopEndPointSamples = 0);
	void detachSample();
	Error truncateFileDownToSize(uint32_t newFileSize);
	Error writeOneCompletedCluster();
	AbsValueFollower envelopeFollower{};
};
