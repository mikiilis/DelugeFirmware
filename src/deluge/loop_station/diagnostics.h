#pragma once
#include "memory_reservation.h"
namespace loop_station {
// Fixed values, sampled only on the serialized foreground/audio owner.
struct DiagnosticSnapshot {
	MemoryProfile profile = kConfiguredMemoryProfile;
	size_t activeBytes = 0, historyBytes = 0, snapshotBytes = 0, gestureBytes = 0, stagingBytes = 0, reservedBytes = 0;
	uintptr_t arenaAddresses[5]{};
	uint64_t callbacks = 0, callbackCycles = 0, maxCallbackCycles = 0, renderedFrames = 0;
	uint64_t pageCopyPages = 0, pageCopyFrames = 0, historyPressure = 0, storageCalls = 0;
	uint64_t capturedFrames[kTrackCount]{}, playbackFrames[kTrackCount]{};
	uint32_t historyPages = 0, snapshotPages = 0;
	uint8_t captureMask = 0, playingMask = 0;
	Status status = Status::Unavailable;
};
class Diagnostics {
public:
	DiagnosticSnapshot snapshot() const { return value_; }
	void configure(const MemoryReservation& memory);
	void callback(uint32_t cycles, uint32_t frames);
	void track(SlotId slot, uint32_t frames, bool capture, bool playing);
	void work(uint64_t pages, uint64_t frames, uint64_t pressure, uint32_t historyPages, uint32_t snapshotPages);
	void storageCall() { ++value_.storageCalls; }
	void status(Status status) { value_.status = status; }

private:
	DiagnosticSnapshot value_{};
};
} // namespace loop_station
