#pragma once
#include "fixture.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include <array>

struct AudioFileManager {
	Cluster* clusterBeingLoaded = nullptr;
	struct Files {
		Error insertElement(Sample*) { return Error::NONE; }
	} audioFiles;
	Error getUnusedAudioRecordingFilePath(String& path, String* temporary, AudioRecordingFolder, uint32_t* number,
	                                      char const*, String*) {
		*number = 1;
		path.set("CLIPS/take.WAV");
		temporary->set("TEMP/take.WAV");
		return Error::NONE;
	}
	std::array<int, 4> highestUsedAudioRecordingNumber{};
	void removeReasonFromCluster(Cluster& cluster, char const*) {
		if (--cluster.numReasonsToBeLoaded < 0)
			throw std::runtime_error("Cluster reason underflow");
		recorder_test::clusterReleases++;
		if (recorder_test::recycleReleasedClusters && cluster.numReasonsToBeLoaded == 0) {
			cluster.sample->clusters.getElement(cluster.clusterIndex)->cluster = nullptr;
			cluster.destroy();
		}
	}
	void deleteUnusedAudioFileFromMemoryIndexUnknown(Sample& sample) {
		if (sample.numReasonsToBeLoaded != 0)
			throw std::runtime_error("Deleting a retained sample");
		sample.~Sample();
		delugeDealloc(&sample);
	}
};
inline AudioFileManager audioFileManager;
