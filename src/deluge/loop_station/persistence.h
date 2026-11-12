#pragma once

#include "config.h"
#include "types.h"

#include <array>
#include <span>

namespace loop_station {

class SongModel;

inline constexpr size_t kCheckpointPathBytes = 80;
inline constexpr unsigned kPersistenceDocumentRows = kTrackCount;

struct SavedPhrase {
	SlotId slot = kInvalidSlot;
	Generation identity = 0;
	FrameIndex frames = 0;
	std::array<char, kCheckpointPathBytes> path{};
};

struct Manifest {
	Generation generation = 0;
	uint8_t phraseCount = 0;
	std::array<SavedPhrase, kTrackCount> phrases{};
};

struct PersistedAction {
	int32_t mode = -1;
	int32_t interval = -1;
};

struct PersistedTrack {
	int32_t id = -1;
	int32_t source = -1;
	int32_t monitoring = -1;
	int32_t recordDurationBars = -1;
	int32_t playback = -1;
	int32_t colour = -1;
	int32_t transposeCents = 0;
};

struct SettingsDocument {
	uint32_t version = 0;
	int32_t recordAction = -1;
	PersistedAction startAction{}, stopAction{};
	uint8_t trackCount = 0;
	PersistedTrack tracks[kPersistenceDocumentRows]{};
	Generation manifestGeneration = 0;
	uint8_t phraseCount = 0;
	SavedPhrase phrases[kPersistenceDocumentRows]{};
	// Legacy values are intentionally accepted by the test adapter and ignored.
	int32_t legacyRecordMode = 0, legacyImmediateStop = 0;
};

struct Admission {
	std::array<bool, kTrackCount> admitted{};
	std::array<SavedPhrase, kTrackCount> phrases{};
	uint8_t count = 0;
	uint8_t omittedCount = 0;
	uint32_t usedPages = 0;
};

enum class SaveState : uint8_t { Clean, Pending, Unsaved, Failed };

// Pure fixed-capacity schema validation and aggregate load admission. Firmware
// Serializer/Deserializer adapters remain in the Song integration layer.
class Persistence {
public:
	static constexpr uint32_t kSchemaVersion = 1;
	static Status decode(const SettingsDocument&, Settings&, Manifest&);
	static Status encode(const Settings&, const Manifest&, SettingsDocument&);
	static Status admitLoadedPhrases(std::span<const SavedPhrase>, MemoryProfile, Admission&);
};

// Allocation-free state owned by Song, independent of the optional runtime
// SongModel. All mutations are explicit value transfers; it retains no runtime
// handles or pointers.
class PersistenceState {
public:
	Status installLoaded(const Settings&, const Manifest&);
	void setLiveSettings(const Settings&, Generation songGeneration);
	Status beginPendingSave(Generation songGeneration, Generation manifestGeneration, uint8_t expectedPhraseCount,
	                        bool replaceManifest);
	Status requestSave(SongModel* model);
	void serviceArchiveCandidates(SongModel& model);
	void cancelPending(SongModel* model);
	void failPending(SongModel* model);
	Status prepareLoadAdmission(MemoryProfile profile);
	Status stageFinalized(const SavedPhrase&);
	bool pendingReady() const;
	Status encodeForSave(SettingsDocument&) const;
	Status encodeForSaveScratch() const;
	SettingsDocument& scratchDocument() const { return scratchDocument_; }
	Status commitPendingAfterDocumentVerified();
	void cancelPending();
	void failPending();
	Status commitPendingAfterDocumentVerified(const std::array<Generation, kTrackCount>& currentRevisions);

	const Settings& liveSettings() const { return liveSettings_; }
	const Settings& committedSettings() const { return committedSettings_; }
	const Manifest& committedManifest() const { return committedManifest_; }
	const Manifest& pendingManifest() const { return pendingManifest_; }
	const Admission& loadAdmission() const { return loadAdmission_; }
	SaveState saveState() const { return saveState_; }
	Generation committedAudioRevision(SlotId slot) const {
		return validSlot(slot) ? committedAudioRevisions_[slot] : 0;
	}
	bool hasCommittedAudioRevision(SlotId slot) const { return validSlot(slot) && committedAudioRevisionMask_[slot]; }
	Generation committedSongGeneration() const { return committedSongGeneration_; }

private:
	Settings liveSettings_ = defaultSettings();
	Settings committedSettings_ = defaultSettings();
	Settings pendingSettings_ = defaultSettings();
	Manifest committedManifest_{};
	Manifest pendingManifest_{};
	Admission loadAdmission_{};
	mutable SettingsDocument scratchDocument_{};
	Generation liveSongGeneration_ = 0;
	Generation committedSongGeneration_ = 0;
	Generation pendingSongGeneration_ = 0;
	uint8_t expectedPhraseCount_ = 0;
	uint8_t archiveRequestMask_ = 0;
	std::array<Generation, kTrackCount> snapshotRevisions_{};
	std::array<Generation, kTrackCount> committedAudioRevisions_{};
	std::array<bool, kTrackCount> committedAudioRevisionMask_{};
	std::array<bool, kTrackCount> representedRevisionMask_{};
	bool pendingActive_ = false;
	bool replaceManifest_ = false;
	bool retainedStale_ = false;
	SaveState saveState_ = SaveState::Clean;
};

} // namespace loop_station
