/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "definitions.h"
#include "io/debug/log.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "storage/cluster/cluster.h"
#include "util/exceptions.h"
#include <cstring>

namespace {
constexpr int32_t maxFileSizeMagnitude = 32;
}

Error SampleRecorder::createNextCluster() {
	int32_t const nextClusterIndex = currentRecordClusterIndex + 1;

	// If this new cluster would actually put us past the 4GB limit...
	if (nextClusterIndex >= (1 << (maxFileSizeMagnitude - Cluster::size_magnitude))) {
		currentRecordClusterIndex = nextClusterIndex;
		currentRecordCluster = nullptr;

		// See if we actually already had any bytes to write into that new cluster we can't have...
		int32_t bytesTilClusterEnd = clusterEndPos - writePos;
		if (bytesTilClusterEnd < 0) {
			numSamplesCaptured--;
			writePos -= (int32_t)recordingNumChannels * 3;
		}

		totalSampleLengthNowKnown(numSamplesCaptured, numSamplesCaptured);

		reachedMaxFileSize = true;
		return Error::MAX_FILE_SIZE_REACHED;
	}

	// The current cluster and any completed clusters remain charged while a write is in
	// progress. writeOneCompletedCluster() reduces this count only after a successful write.
	if (loopStationLinearCapture_ && loopUnwrittenClusterCount_ >= kMaxLoopUnwrittenClusters) {
		return Error::INSUFFICIENT_RAM;
	}

	Cluster* oldRecordCluster = currentRecordCluster;
	currentRecordClusterIndex = nextClusterIndex; // Mark record-cluster we were on as finished
	currentRecordCluster = nullptr; // Until the new cluster is allocated, this means this index does not exist.

	Error error = sample->clusters.insertSampleClustersAtEnd(1);
	if (error != Error::NONE) {
		return error;
	}

	currentRecordCluster = sample->clusters.getElement(currentRecordClusterIndex)
	                           ->getCluster(sample, currentRecordClusterIndex, CLUSTER_DONT_LOAD);

	// If couldn't allocate cluster (would normally only happen if no SD card present so recording only to RAM)
	if (!currentRecordCluster) {
		D_PRINTLN("SampleRecorder::createNextCluster() fail");
		return Error::INSUFFICIENT_RAM;
	}

	if (currentRecordCluster->numReasonsHeldBySampleRecorder) {
		FREEZE_WITH_ERROR("E362");
	}
	currentRecordCluster->numReasonsHeldBySampleRecorder++;
	if (loopStationLinearCapture_) {
		loopUnwrittenClusterCount_++;
	}

	// Copy those extra bytes from the end of the old record cluster to the start of the new cluster.
	memcpy(currentRecordCluster->data, &oldRecordCluster->data[Cluster::size], 5);

	int32_t bytesOvershot = writePos - clusterEndPos;
	currentRecordCluster->loaded = true;
	writePos = currentRecordCluster->data + bytesOvershot;
	clusterEndPos = currentRecordCluster->data + Cluster::size;

	return Error::NONE;
}
