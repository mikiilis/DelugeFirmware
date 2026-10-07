#pragma once
#include "fixture.h"
#include "model/sample/sample_cluster.h"
#include "storage/cluster/cluster.h"
#include "util/d_string.h"
#include <array>
#include <stdexcept>

class Sample {
public:
	String filePath;
	String tempFilePathForRecording;
	uint32_t audioDataStartPosBytes = 44;
	uint32_t audioDataLengthBytes = 0;
	uint32_t fileLoopEndSamples = 0;
	uint32_t lengthInSamples = 0;
	uint32_t numChannels = 1;
	uint32_t byteDepth = 3;
	Sample() { recorder_test::liveSamples++; }
	~Sample() { recorder_test::liveSamples--; }
	struct Clusters {
		std::array<SampleCluster, 16> entries{};
		int count = 0;
		Error insertSampleClustersAtEnd(int n) {
			if (recorder_test::clusterInsertFails)
				return Error::INSUFFICIENT_RAM;
			if (count + n > entries.size())
				return Error::INSUFFICIENT_RAM;
			count += n;
			return Error::NONE;
		}
		int getNumElements() const { return count; }
		SampleCluster* getElement(int index) { return &entries.at(index); }
		void deleteAtIndex(int index, int n) {
			// Production has already destructed the removed tail. Reconstruct empty
			// entries because this fixture uses a fixed array with automatic destruction.
			if (index + n != count)
				throw std::runtime_error("Not a tail deletion");
			for (int i = index; i < count; ++i)
				new (&entries[i]) SampleCluster();
			count -= n;
		}
	} clusters;
	int numReasonsToBeLoaded = 0;
	void addReason() { numReasonsToBeLoaded++; }
	void removeReason(char const*) {
		if (--numReasonsToBeLoaded < 0)
			throw std::runtime_error("Sample reason underflow");
	}
	Error initialize(int count) {
		if (recorder_test::failure == recorder_test::Failure::SAMPLE_INITIALIZE)
			return Error::INSUFFICIENT_RAM;
		clusters.count = count;
		return Error::NONE;
	}
};
