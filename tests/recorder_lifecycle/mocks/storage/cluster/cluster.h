#pragma once
#include "fixture.h"
class Sample;
class Cluster {
public:
	static constexpr size_t size = 32768;
	static constexpr unsigned size_magnitude = 15;
	Cluster() { recorder_test::liveClusters++; }
	~Cluster() { recorder_test::liveClusters--; }
	void destroy() {
		recorder_test::destroyedClusters++;
		delete this;
	}
	Sample* sample = nullptr;
	uint32_t clusterIndex = 0;
	bool loaded = true;
	int numReasonsHeldBySampleRecorder = 0;
	int numReasonsToBeLoaded = 0;
	alignas(4) char data[size + 8]{};
};
