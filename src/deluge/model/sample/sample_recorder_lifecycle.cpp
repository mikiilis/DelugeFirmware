/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "definitions.h"
#include "memory/general_memory_allocator.h"
#include "model/output.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"
#include <algorithm>
#include <new>

// Bounded RAM-only trace, inspectable in a debugger. Never allocate or write to SD/audio logs.
#if ALPHA_OR_BETA_VERSION
struct RecorderLifecycleTrace {
	SampleRecorder const* recorder;
	Sample const* sample;
	Output const* output;
	uint32_t captured;
	int32_t reasons;
	RecorderStatus status;
	Error terminalError;
	SampleRecorder::LifecycleEvent event;
	bool commitPending;
	bool pointerHeld;
	Cluster const* cluster;
	int32_t clusterReasons;
	int32_t recorderClusterReasons;
	uint32_t clusterIndex;
	uint32_t targetFileSize;
	int32_t firstUnwritten;
	int32_t currentRecord;
	uint8_t retainedStartupClusters;
	uint8_t loopStationSlot;
	AudioInputChannel source;
	uint64_t audioDataLength;
	uint32_t clusterCount;
	bool clusterLoading;
};
// Volatile retains the debugger-visible writes even under Release LTO.
volatile RecorderLifecycleTrace recorderLifecycleTrace[64]{};
volatile uint32_t recorderLifecycleTraceSequence = 0;
#endif

void SampleRecorder::traceLifecycle(LifecycleEvent event, Cluster const* cluster, uint32_t targetFileSize) const {
#if ALPHA_OR_BETA_VERSION
	auto sequence = recorderLifecycleTraceSequence;
	auto& entry = recorderLifecycleTrace[sequence % 64];
	entry.recorder = this;
	entry.sample = sample;
	entry.output = outputRecordingFrom;
	entry.captured = numSamplesCaptured;
	entry.reasons = sample ? sample->numReasonsToBeLoaded : 0;
	entry.status = status;
	entry.terminalError = terminalError;
	entry.event = event;
	entry.commitPending = commitPending_;
	entry.pointerHeld = pointerHeldElsewhere;
	entry.cluster = cluster;
	entry.clusterReasons = cluster ? cluster->numReasonsToBeLoaded : 0;
	entry.recorderClusterReasons = cluster ? cluster->numReasonsHeldBySampleRecorder : 0;
	entry.clusterIndex = cluster ? cluster->clusterIndex : 0;
	entry.targetFileSize = targetFileSize;
	entry.firstUnwritten = firstUnwrittenClusterIndex;
	entry.currentRecord = currentRecordClusterIndex;
	entry.retainedStartupClusters = retainedStartupClusters_;
	entry.loopStationSlot = diagnosticLoopStationSlot;
	entry.source = mode;
	entry.audioDataLength = sample ? sample->audioDataLengthBytes : 0;
	entry.clusterCount = sample ? sample->clusters.getNumElements() : 0;
	entry.clusterLoading = cluster && cluster == audioFileManager.clusterBeingLoaded;
	recorderLifecycleTraceSequence = sequence + 1;
#endif
}

bool Output::removeRecorder(SampleRecorder* expected) {
	if (!recorder || (expected && recorder != expected)) {
		return false;
	}
	auto* detached = recorder;
	recorder = nullptr;
	detached->removeFromOutput();
	return true;
}

SampleRecorder::~SampleRecorder() {
	traceLifecycle(LifecycleEvent::DESTROYED);
	if (sample != nullptr) {
		detachSample();
		if (!haveAddedSampleToArray && sample->numReasonsToBeLoaded == 0) {
			// Setup can succeed before output attachment fails. This Sample was never registered.
			sample->~Sample();
			delugeDealloc(sample);
		}
		sample = nullptr;
	}
	if (outputRecordingFrom) {
		outputRecordingFrom->removeRecorder(this);
	}
}

// Release only explicitly retained startup reasons, never reader or loader reasons.
void SampleRecorder::releaseRetainedStartupClusters(uint32_t firstIndex) {
	static_assert(kNumClustersLoadedAhead <= 8);
	for (uint32_t l = firstIndex; l < kNumClustersLoadedAhead; ++l) {
		if (retainedStartupClusters_ & (1u << l)) {
			Cluster* cluster = sample->clusters.getElement(l)->cluster;

			// Some bug-hunting
			if (!cluster || !cluster->numReasonsHeldBySampleRecorder) {
				FREEZE_WITH_ERROR("E345");
			}
			retainedStartupClusters_ &= ~(1u << l);
			cluster->numReasonsHeldBySampleRecorder--;

			audioFileManager.removeReasonFromCluster(*cluster, "E257");
		}
	}
}

// Used on destruction and abort. The caller must clear sample after detaching it.
void SampleRecorder::detachSample() {
	releaseRetainedStartupClusters();

	int32_t removeForClustersUntilIndex = currentRecordClusterIndex;
	if (currentRecordCluster) {
		removeForClustersUntilIndex++; // If there's a currentRecordCluster (usually will be if aborting), need to
		                               // remove its "reason" too
	}

	while (firstUnwrittenClusterIndex < removeForClustersUntilIndex) {
		Cluster* cluster = sample->clusters.getElement(firstUnwrittenClusterIndex)->cluster;

		if (!cluster) {
			FREEZE_WITH_ERROR("E363");
		}

		// Some bug-hunting
		if (!cluster->numReasonsHeldBySampleRecorder) {
			FREEZE_WITH_ERROR("E346");
		}
		cluster->numReasonsHeldBySampleRecorder--;

		audioFileManager.removeReasonFromCluster(*cluster, "E249");
		firstUnwrittenClusterIndex++;
	}

	sample->removeReason("E400");
}

Error SampleRecorder::initializeRecordingSample() {
	// Didn't seem to make a difference forcing this into local RAM
	void* sample_memory = GeneralMemoryAllocator::get().allocStealable(sizeof(Sample));
	if (sample_memory == nullptr) {
		return Error::INSUFFICIENT_RAM;
	}

	sample = new (sample_memory) Sample;
	sample->addReason(); // Must call this so it's protected from stealing, before we call initialize().
	Error error = sample->initialize(1);
	if (error != Error::NONE) {
gotError:
		sample->~Sample();
		delugeDealloc(sample_memory);
		sample = nullptr;
		return error;
	}

	currentRecordCluster =
	    sample->clusters.getElement(0)->getCluster(sample, 0, CLUSTER_DONT_LOAD); // Adds a "reason" to it, too
	if (!currentRecordCluster) {
		error = Error::INSUFFICIENT_RAM;
		goto gotError;
	}

	// Bug hunting - newly gotten Cluster
	if (currentRecordCluster->numReasonsHeldBySampleRecorder) {
		FREEZE_WITH_ERROR("E360");
	}
	currentRecordCluster->numReasonsHeldBySampleRecorder++;
	currentRecordClusterIndex = 0;

	return Error::NONE;
}

void SampleRecorder::retainForCommit() {
	commitPending_ = true;
	pointerHeldElsewhere = true;
	traceLifecycle(LifecycleEvent::RETAINED);
}

void SampleRecorder::releaseCommit(bool success) {
	if (!commitPending_) {
		return;
	}
	commitPending_ = false;
	pointerHeldElsewhere = false;
	if (success) {
		status = RecorderStatus::AWAITING_DELETION;
	}
	else {
		abort();
	}
	traceLifecycle(LifecycleEvent::RELEASED);
}

void SampleRecorder::abort() {
	if (status != RecorderStatus::ABORTED) {
		status = RecorderStatus::ABORTED;
		traceLifecycle(LifecycleEvent::ABORTED);
	}
}

Error SampleRecorder::cleanupAbortedRecording() {
	// Holding the recorder pointer alone is insufficient: the clip/history can own its Sample too.
	if (commitPending_) {
		return Error::NONE;
	}
	traceLifecycle(LifecycleEvent::CLEANED);
	// Close before unlinking; a failed write may have left the file open.
	file.reset();
	if (sample != nullptr) { // This might get called multiple times, so check we haven't already detached it.

		// Note: if this abort() is due to a song-swap (loading a different song),
		// then samples is about to be searched for temp ones to delete, and we'll need to have deleted it here
		// before that trips over us. Previously caused an E281. So, for that to happen,
		// SampleManager::deleteAnyTempRecordedSamplesFromMemory() (indirectly) calls us here first.

		detachSample(); // Does not set sample to NULL - we do that below

#if ALPHA_OR_BETA_VERSION
		// Commit owners must release provisional references before allowing cleanup.
		if (sample->numReasonsToBeLoaded) {
			FREEZE_WITH_ERROR("E282");
		}
#endif

		if (haveAddedSampleToArray) { // We only add it to the array when the file is created.
			audioFileManager.deleteUnusedAudioFileFromMemoryIndexUnknown(*sample);
		}
		else {
			// Failed/very short takes may abort before registration with the file manager.
			sample->~Sample();
			delugeDealloc(sample);
		}

		sample = nullptr; // So we don't try to detach it again when we're destructed
	}

	// Delete the file if one was created
	if (!filePathCreated.isEmpty()) {

		FRESULT result = f_unlink(filePathCreated.get());

		// If this was the most recent recording in this category, tick the counter backwards - so long as
		// either the delete was successful or it was for an AudioClip, which means the file is in the TEMP folder
		// and can be overwritten anyway
		if (result == FR_OK || folderID == AudioRecordingFolder::CLIPS) {
			if (audioFileManager.highestUsedAudioRecordingNumber[util::to_underlying(folderID)] == audioFileNumber) {
				audioFileManager.highestUsedAudioRecordingNumber[util::to_underlying(folderID)]--;
			}
		}
		filePathCreated.clear();
	}

	// Normally we now just await deletion - except if a pointer is still being held elsewhere - which I think can
	// only happen from AudioRecorder. Or if the abort comes from a failure within this class and the AudioClip
	// hasn't realised yet?
	if (!pointerHeldElsewhere) {
		status = RecorderStatus::AWAITING_DELETION;
	}
	return Error::NONE;
}
