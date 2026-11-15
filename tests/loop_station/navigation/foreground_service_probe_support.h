#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
// Minimal stubs mirroring exactly what the extracted production
// Song::serviceLoopStationArchive body touches. They exist only so the real
// production function can be compiled and run in the host test.
namespace loop_station {
enum class Status : uint8_t {
	Ok,
	Invalid,
	Busy,
	NoMemory,
	SourceUnavailable,
	Stale,
	Unavailable,
	StorageError,
	Cancelled
};
}
struct ProbeModel {
	int archiveCalls = 0;
	int navigationCalls = 0;
	int candidateCalls = 0;
	uint32_t navigationBudget = 0;
	loop_station::Status serviceArchive(uint32_t) {
		++archiveCalls;
		return loop_station::Status::Ok;
	}
	void serviceHistoryNavigation(uint32_t budget) {
		++navigationCalls;
		navigationBudget = budget;
	}
};
struct ProbePersistence {
	int candidateCalls = 0;
	void serviceArchiveCandidates(ProbeModel&) { ++candidateCalls; }
};
struct ProbeOwner {
	ProbeModel model;
	int restoreCalls = 0;
	void serviceCheckpointRestore() { ++restoreCalls; }
};
struct ProbeSong {
	ProbeOwner* loopStationOwner_ = nullptr;
	ProbePersistence persistence;
	ProbePersistence& loopStationPersistence() { return persistence; }
	void serviceLoopStationArchive(uint32_t maxBytes);
};
