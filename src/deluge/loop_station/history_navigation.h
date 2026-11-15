#pragma once
#include "types.h"
namespace loop_station {
// Stable identity for a confirmed Undo/Redo request. A raw clip or model pointer
// is never a sufficient identity: the model identity is monotonic per constructed
// SongModel, the slot generation changes on rebind/replacement, the engine history
// epoch changes only on real capture/session/phrase transitions, and the transport
// epoch changes when the observed running/stopped state changes. Together they let
// a stale confirmation be rejected before any audio change.
struct HistoryTarget {
	SlotId slot = kInvalidSlot;
	bool redo = false;
	Generation modelIdentity = 0, slotGeneration = 0, historyEpoch = 0, transportEpoch = 0;
};
enum class HistoryNavigationState : uint8_t { Idle, Applying, Complete, Failed };
struct HistoryNavigationResult {
	RestoreId id{};
	SlotId slot = kInvalidSlot;
	bool redo = false;
	HistoryNavigationState state = HistoryNavigationState::Idle;
	Status status = Status::Ok;
	bool pending() const { return state == HistoryNavigationState::Idle || state == HistoryNavigationState::Applying; }
};
} // namespace loop_station
