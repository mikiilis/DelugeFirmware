#pragma once
#include "deluge/loop_station/persistence.h"
#include "deluge/loop_station/song_model.h"
#include <array>

namespace loop_station {
// Production FirmwareOwner checkpoint-mask methods compiled against the real
// SongModel. The owner only owns the admission mask and request handoff; the
// model owns every real checkpoint read, restore and audio commit.
struct CheckpointOwnerProbe {
	explicit CheckpointOwnerProbe(SongModel& model_) : model(model_) {}
	SongModel& model;
	std::array<SavedPhrase, kTrackCount> restorePhrases{};
	uint8_t restoreMask = 0;
	RestoreId activeRestore{};
	SlotId activeRestoreSlot = kInvalidSlot;
	void beginCheckpointRestores(const PersistenceState& persistence);
	void startNextRestore();
	void serviceCheckpointRestore();
};
} // namespace loop_station
