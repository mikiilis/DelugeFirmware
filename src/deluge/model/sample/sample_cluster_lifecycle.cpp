/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "io/debug/log.h"
#include "model/sample/sample_cluster.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"

#if ALPHA_OR_BETA_VERSION
// Preserved at the assertion site even if the recorder event ring has wrapped.
// The debugger call stack distinguishes whole-Sample deletion from tail trimming.
struct SampleClusterDestructionSnapshot {
	Cluster const* cluster;
	Sample const* sample;
	uint32_t index;
	int32_t reasons;
	int32_t recorderReasons;
	bool loading;
};
volatile SampleClusterDestructionSnapshot sampleClusterDestructionFailure{};
#endif

SampleCluster::~SampleCluster() {
	if (cluster) {

#if ALPHA_OR_BETA_VERSION
		int32_t numReasonsToBeLoaded = cluster->numReasonsToBeLoaded;
		if (cluster == audioFileManager.clusterBeingLoaded) {
			numReasonsToBeLoaded--;
		}

		if (numReasonsToBeLoaded) {
			sampleClusterDestructionFailure.cluster = cluster;
			sampleClusterDestructionFailure.sample = cluster->sample;
			sampleClusterDestructionFailure.index = cluster->clusterIndex;
			sampleClusterDestructionFailure.reasons = cluster->numReasonsToBeLoaded;
			sampleClusterDestructionFailure.recorderReasons = cluster->numReasonsHeldBySampleRecorder;
			sampleClusterDestructionFailure.loading = cluster == audioFileManager.clusterBeingLoaded;
			D_PRINTLN("uh oh, some reasons left...  %d", numReasonsToBeLoaded);

			// Bay_Mud got this, and thinks a FlashAir card might have been a catalyst. It still "shouldn't" be able to
			// happen though.
			FREEZE_WITH_ERROR("E036");
		}
#endif
		cluster->destroy();
	}
}
