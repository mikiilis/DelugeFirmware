#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
// Minimal stubs mirroring exactly what the extracted production
// Song::serviceLoopStationArchive and FirmwareOwner checkpoint-mask methods touch.
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
inline constexpr unsigned kTrackCount = 8;
using SlotId = uint8_t;
inline constexpr SlotId kInvalidSlot = 255;
using Generation = uint64_t;
struct RestoreId {
	Generation generation = 0;
	bool valid() const { return generation != 0; }
};
struct SavedPhrase {
	Generation identity = 0;
};
struct PersistenceState {
	struct LoadAdmission {
		bool admitted[kTrackCount]{};
		SavedPhrase phrases[kTrackCount]{};
	} admission;
	const LoadAdmission& loadAdmission() const { return admission; }
};
} // namespace loop_station
using loop_station::Generation;
using loop_station::RestoreId;
using loop_station::SavedPhrase;
using loop_station::SlotId;
using loop_station::Status;
struct ProbeModel {
	Status ensureStatus[loop_station::kTrackCount]{};
	Status requestStatus[loop_station::kTrackCount]{};
	Status serviceStatus = Status::Busy;
	Status commitStatus = Status::Ok;
	unsigned ensureCalls[loop_station::kTrackCount]{};
	unsigned requestCalls[loop_station::kTrackCount]{};
	unsigned serviceCalls = 0, commitCalls = 0;
	unsigned cancelCalls[loop_station::kTrackCount]{};
	Generation sequence = 0;
	int archiveCalls = 0;
	int navigationCalls = 0;
	uint32_t navigationBudget = 0;
	Status serviceArchive(uint32_t) {
		++archiveCalls;
		return Status::Ok;
	}
	void serviceHistoryNavigation(uint32_t budget) {
		++navigationCalls;
		navigationBudget = budget;
	}
	Status ensureBound(SlotId slot) {
		++ensureCalls[slot];
		return ensureStatus[slot];
	}
	Status requestCheckpointRestore(SlotId slot, Generation, RestoreId& result) {
		++requestCalls[slot];
		result = {};
		if (requestStatus[slot] != Status::Ok)
			return requestStatus[slot];
		result = {++sequence};
		return Status::Ok;
	}
	Status serviceRestore(RestoreId, uint32_t) {
		++serviceCalls;
		return serviceStatus;
	}
	Status commitRestore(RestoreId) {
		++commitCalls;
		return commitStatus;
	}
	void cancelRestore(SlotId slot) { ++cancelCalls[slot]; }
};
struct ProbePersistence {
	int candidateCalls = 0;
	void serviceArchiveCandidates(ProbeModel&) { ++candidateCalls; }
};
struct ProbeOwner {
	ProbeModel model;
	std::array<SavedPhrase, loop_station::kTrackCount> restorePhrases{};
	uint8_t restoreMask = 0;
	RestoreId activeRestore{};
	SlotId activeRestoreSlot = 255;
	// Bodies are extracted unchanged from FirmwareOwner.
	void beginCheckpointRestores(const loop_station::PersistenceState& persistence);
	void startNextRestore();
	void serviceCheckpointRestore();
};
struct ProbeSong {
	ProbeOwner* loopStationOwner_ = nullptr;
	ProbePersistence persistence;
	ProbePersistence& loopStationPersistence() { return persistence; }
	void serviceLoopStationArchive(uint32_t maxBytes);
};
