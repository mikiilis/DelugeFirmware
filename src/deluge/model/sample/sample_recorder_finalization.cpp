/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "definitions.h"
#include "gui/ui/browser/sample_browser.h"
#include "io/debug/log.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "processing/engines/audio_engine.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"
#include "storage/storage_manager.h"
#include <algorithm>

extern "C" {
#include "fatfs/diskio.h"
}

Error SampleRecorder::writeAnyCompletedClusters() {
	while (firstUnwrittenClusterIndex < currentRecordClusterIndex) {

		Error error = writeOneCompletedCluster();

		// If there was an error, we can only return now after removing that reason, because we'd already incremented
		// firstUnwrittenClusterIndex, and we can't leave that incremented without removing the reason
		if (error != Error::NONE) {
			return error;
		}
	}

	return Error::NONE;
}

Error SampleRecorder::writeOneCompletedCluster() {
	int32_t writingClusterIndex = firstUnwrittenClusterIndex;

#if ALPHA_OR_BETA_VERSION
	// Trying to pin down E347 which Leo got, below
	Cluster* cluster = sample->clusters.getElement(writingClusterIndex)->cluster;
	if (!cluster->numReasonsHeldBySampleRecorder) {
		FREEZE_WITH_ERROR("E374");
	}
#endif

	firstUnwrittenClusterIndex++; // Have to increment this before writing, cos while writing, the audio routine will be
	                              // called, and we need to be counting this cluster as "written", as in too late for it
	                              // to be modified (by writing a final length to it)
	if (keepingReasonsForFirstClusters && writingClusterIndex < kNumClustersLoadedAhead) {
		retainedStartupClusters_ |= 1u << writingClusterIndex;
	}

	Error error = writeCluster(writingClusterIndex, Cluster::size);

	// We no longer have a reason to require this Cluster to be kept in memory
	if (!keepingReasonsForFirstClusters || writingClusterIndex >= kNumClustersLoadedAhead) {
		Cluster* cluster = sample->clusters.getElement(writingClusterIndex)->cluster;

		// Some bug-hunting
		if (!cluster->numReasonsHeldBySampleRecorder) {
			// Leo got!!! And Vinz, and keyman. May be solved now that fixed so detachSample() doesn't get called during
			// card routine.
			FREEZE_WITH_ERROR("E347");
		}
		cluster->numReasonsHeldBySampleRecorder--;

		audioFileManager.removeReasonFromCluster(*cluster, "E015");
	}

	// If there was an error, we can only return now after removing that reason, because we'd already incremented
	// firstUnwrittenClusterIndex, and we can't leave that incremented without removing the reason
	return error;
}

Error SampleRecorder::finalizeRecordedFile() {

	if (ALPHA_OR_BETA_VERSION && (status == RecorderStatus::ABORTED || hadCardError)) {
		FREEZE_WITH_ERROR("E273");
	}

	D_PRINTLN("finalizing");

	// In the very rare case where we've already got between 1 and 5 bytes overhanging the end of our current cluster,
	// we need to allocate a new one right now
	int32_t bytesTilClusterEnd = clusterEndPos - writePos;
	if (bytesTilClusterEnd < 0) {
		Error error = createNextCluster();

		if (error == Error::MAX_FILE_SIZE_REACHED) {
		} // So incredibly unlikely. But no real problem - we maybe just lose a byte or two

		else if (error != Error::NONE) {
			return error;
		}
		else { // No error
			// Having just created a new cluster, there'll be one more completed one to write
			error = writeAnyCompletedClusters();
			if (error != Error::NONE) {
				return error;
			}
		}
	}

	// And we probably need to write some of the final cluster(s) to file. (If it's NULL, it means that it couldn't be
	// created, cos or RAM or file size limit.)
	if (currentRecordCluster) {

		int32_t bytesToWrite = writePos - currentRecordCluster->data;
		if (bytesToWrite > 0) { // Will always be true
			Error error = writeCluster(currentRecordClusterIndex, bytesToWrite);
			if (error != Error::NONE) {
				return error;
			}
		}

		firstUnwrittenClusterIndex++;
		if (keepingReasonsForFirstClusters && currentRecordClusterIndex < kNumClustersLoadedAhead) {
			retainedStartupClusters_ |= 1u << currentRecordClusterIndex;
		}

		// Having incremented firstUnwrittenClusterIndex, we need to remove the "reason" for that final cluster.
		// Normally that happens in writeAnyCompletedClusters(), but well this cluster wasn't "complete" so we're doing
		// the whole thing here instead
		if (!keepingReasonsForFirstClusters || currentRecordClusterIndex >= kNumClustersLoadedAhead) {

			// Some bug-hunting
			if (!currentRecordCluster->numReasonsHeldBySampleRecorder) {
				FREEZE_WITH_ERROR("E348");
			}
			currentRecordCluster->numReasonsHeldBySampleRecorder--;

			audioFileManager.removeReasonFromCluster(*currentRecordCluster, "E047");
		}
		currentRecordClusterIndex++;    // We've finished with that cluster
		currentRecordCluster = nullptr; // But currentRecordClusterIndex now refers to a cluster that'll never exist
	}

	uint32_t idealFileSizeBeforeAction = sample->audioDataStartPosBytes + sample->audioDataLengthBytes;
	uint32_t dataLengthBeforeAction = sample->audioDataLengthBytes;

	// Figure out what processing needs to happen on the recorded audio
	MonitoringAction action = MonitoringAction::NONE;
	int32_t lshiftAmount = 0;

	if (allowFileAlterationAfter
	    && idealFileSizeBeforeAction <= 67108864) { // Arbitrarily, don't alter files bigger than 64MB
		if (recordingNumChannels == 1) {
			action = MonitoringAction::NONE;
		}
		else {
			// If R is really quiet or is nearly identical to L, delete R
			if (inputHasNoRightChannel() || recordSumLMinusR < (recordSumL >> 6)) {
				D_PRINTLN("removing right channel");
				action = MonitoringAction::REMOVE_RIGHT_CHANNEL;
			}

			// Or, if R is the differential signal of L, do that
			else if (mode < AUDIO_INPUT_CHANNEL_FIRST_INTERNAL_OPTION && AudioEngine::lineInPluggedIn
			         && inputLooksDifferential()) {
				D_PRINTLN("subtracting right channel");
				action = MonitoringAction::SUBTRACT_RIGHT_CHANNEL;
			}

			else {
				D_PRINTLN("keeping right channel");
				action = MonitoringAction::NONE;
			}
		}

		uint32_t maxPeak;
		if (action == MonitoringAction::SUBTRACT_RIGHT_CHANNEL) {
			maxPeak = -1 - recordPeakLMinusR;
		}
		else {
			maxPeak = -1 - std::min(recordPeakL, recordPeakR);
		}

		if (allowNormalization) {
			for (lshiftAmount = 0; ((uint32_t)2147483648 >> (lshiftAmount + 1)) > maxPeak; lshiftAmount++) {}
		}
	}
	uint32_t dataLengthAfterAction =
	    action != MonitoringAction::NONE ? (dataLengthBeforeAction >> 1) : dataLengthBeforeAction;

	// TODO: in a perfect world, where we're not deleting a channel, we'd go backwards from the last Cluster, because
	// that's the most likely to still be in memory

	// If some processing of the recorded audio data needs to happen...
	if (lshiftAmount || action != MonitoringAction::NONE) {
		auto closed = this->file->close();
		if (!closed) {
			return Error::SD_CARD;
		}

		Error error = alterFile(action, lshiftAmount, idealFileSizeBeforeAction, dataLengthAfterAction);
		if (error != Error::NONE) {
			return error;
		}
	}

	// Or if no action or shifting was required...
	else {

		// If we made the file too long, because we then compensated for button latency and are throwing away the last
		// little bit, then truncate it
		if (capturedTooMuch) {
			D_PRINTLN("truncating");
			uint32_t correctLength =
			    sample->audioDataStartPosBytes
			    + sample->audioDataLengthBytes; // These were written to in totalSampleLengthNowKnown().
			Error error = truncateFileDownToSize(correctLength);
			if (error != Error::NONE) {
				return error;
			}
		}

		auto closed = this->file->close();
		if (!closed) {
			return Error::SD_CARD;
		}

		// If the actual audio data length we ended up with is not the same as was written in the headers in the first
		// cluster (very likely; various reasons)
		if (sample->audioDataLengthBytes != audioDataLengthBytesAsWrittenToFile
		    || (recordingExtraMargins && sample->fileLoopEndSamples != loopEndSampleAsWrittenToFile)) {

			// Update data length as written in first cluster
			SampleCluster* firstSampleCluster = sample->clusters.getElement(0);
			Cluster* cluster =
			    firstSampleCluster->getCluster(sample, 0, CLUSTER_LOAD_IMMEDIATELY); // Remember, this adds a "reason"
			if (!cluster) {
				return Error::SD_CARD;
			}
			if (cluster) {

				// Bug hunting - newly gotten Cluster
				cluster->numReasonsHeldBySampleRecorder++;

				// Do a last-ditch check that the SD address doesn't look invalid
				if (firstSampleCluster->sdAddress == 0) {
					FREEZE_WITH_ERROR("E268");
				}
				if ((firstSampleCluster->sdAddress - fileSystem.database) & (fileSystem.csize - 1)) {
					FREEZE_WITH_ERROR("E269");
				}

				audioDataLengthBytesAsWrittenToFile = sample->audioDataLengthBytes;
				loopEndSampleAsWrittenToFile = sample->fileLoopEndSamples;
				updateDataLengthInFirstCluster(cluster);

				// Write just that one first sector back to the card
				DRESULT headerWriteResult = disk_write(0, (BYTE*)cluster->data, firstSampleCluster->sdAddress, 1);

				// Some bug-hunting
				if (!cluster->numReasonsHeldBySampleRecorder) {
					FREEZE_WITH_ERROR("E349");
				}
				cluster->numReasonsHeldBySampleRecorder--;

				audioFileManager.removeReasonFromCluster(*cluster, "E026");
				if (headerWriteResult != RES_OK) {
					return Error::SD_CARD;
				}
			}
		}
	}

	sample->numChannels = (action != MonitoringAction::NONE || recordingNumChannels == 1) ? 1 : 2;
	sample->lengthInSamples = dataLengthAfterAction / (sample->byteDepth * sample->numChannels);
	sample->audioDataLengthBytes =
	    sample->lengthInSamples
	    * (sample->byteDepth
	       * sample->numChannels); // Ensure whole number of samples (surely it already would be though?)

	if (sample->tempFilePathForRecording.isEmpty()) {
		sampleBrowser.lastFilePathLoaded.set(&sample->filePath);
	}

	return Error::NONE;
}

void SampleRecorder::updateDataLengthInFirstCluster(Cluster* cluster) {
	uint32_t data32;

	// Write top-level RIFF chunk size
	*(uint32_t*)&cluster->data[4] = audioDataLengthBytesAsWrittenToFile + sample->audioDataStartPosBytes - 8;

	// Write data chunk size
	*(uint32_t*)&cluster->data[sample->audioDataStartPosBytes - 4] = audioDataLengthBytesAsWrittenToFile;

	if (recordingExtraMargins) {
		// Write loop end point
		*(uint32_t*)&cluster->data[92] = loopEndSampleAsWrittenToFile;
	}
}

// You must still have the file open when you call this
Error SampleRecorder::truncateFileDownToSize(uint32_t newFileSize) {
	// Only completed capture/writes may discard clusters. Validate before unsigned
	// rounding, and preserve the WAV header even for an empty data chunk.
	if (!sample || !file || !newFileSize || newFileSize < sample->audioDataStartPosBytes || currentRecordCluster
	    || firstUnwrittenClusterIndex != currentRecordClusterIndex) {
		traceLifecycle(LifecycleEvent::TRIM_REJECTED, nullptr, newFileSize);
		return Error::BUG;
	}
	uint32_t numClustersAfterAction = ((newFileSize - 1) >> Cluster::size_magnitude) + 1;
	if (numClustersAfterAction > static_cast<uint32_t>(sample->clusters.getNumElements())) {
		traceLifecycle(LifecycleEvent::TRIM_REJECTED, nullptr, newFileSize);
		return Error::BUG;
	}
	releaseRetainedStartupClusters(numClustersAfterAction);
	// Re-fetch descriptors each time: releasing a reason or yielding to storage may
	// recycle a Cluster. A loader reason also prevents physical truncation.
	auto tailIsUnused = [&] {
		for (int32_t i = numClustersAfterAction; i < sample->clusters.getNumElements(); ++i) {
			auto* cluster = sample->clusters.getElement(i)->cluster;
			traceLifecycle(LifecycleEvent::TRIM_CHECK, cluster, newFileSize);
			if (cluster
			    && (cluster->numReasonsToBeLoaded || cluster->numReasonsHeldBySampleRecorder
			        || cluster == audioFileManager.clusterBeingLoaded)) {
				traceLifecycle(LifecycleEvent::TRIM_REJECTED, cluster, newFileSize);
				return false;
			}
		}
		return true;
	};
	if (!tailIsUnused()) {
		return Error::BUG;
	}

	// Truncate file size
	auto seeked = file->lseek(newFileSize);
	if (!seeked) {
		return Error::SD_CARD;
	}
	if (!tailIsUnused()) {
		return Error::BUG;
	}
	auto truncated = file->truncate();
	if (!truncated) {
		return Error::SD_CARD;
	}
	if (!tailIsUnused()) {
		return Error::BUG;
	}
	int32_t numToDelete = sample->clusters.getNumElements() - numClustersAfterAction;
	if (numToDelete > 0) {
		for (int32_t i = numClustersAfterAction; i < sample->clusters.getNumElements(); ++i) {
			sample->clusters.getElement(i)->~SampleCluster();
		}
		sample->clusters.deleteAtIndex(numClustersAfterAction, numToDelete);
	}
	traceLifecycle(LifecycleEvent::TRIM_COMPLETE, nullptr, newFileSize);

	return Error::NONE;
}
