#include "persistence.h"
#include "song_model.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace loop_station {
namespace {
bool enumValue(int32_t value, int32_t max) {
	if (value < 0 || value > max)
		return false;
	return true;
}

bool validPath(const SavedPhrase& phrase) {
	const auto end = std::find(phrase.path.begin(), phrase.path.end(), '\0');
	const auto length = static_cast<size_t>(end - phrase.path.begin());
	if (length != 12 || phrase.path[0] != 'L' || phrase.path[1] < '1' || phrase.path[1] > '8' || phrase.path[8] != '.'
	    || std::memcmp(phrase.path.data() + 8, ".WAV", 4) != 0)
		return false;
	for (size_t i = 2; i < 8; ++i) {
		const char c = phrase.path[i];
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
			return false;
	}
	return true;
}

bool validPhrase(const SavedPhrase& phrase) {
	return validSlot(phrase.slot) && phrase.identity && phrase.frames && validPath(phrase)
	       && phrase.path[1] == static_cast<char>('1' + phrase.slot);
}

bool validAction(PersistedAction persisted, ActionSettings& out) {
	bool valid = true;
	if (enumValue(persisted.mode, int(ActionMode::Synced)) && enumValue(persisted.interval, 11)) {
		out.mode = static_cast<ActionMode>(persisted.mode);
		out.interval = static_cast<Interval>(persisted.interval);
	}
	else
		valid = false;
	return valid;
}

bool validSettings(const Settings& settings) {
	if (static_cast<unsigned>(settings.recordAction) > static_cast<unsigned>(RecordAction::REC_DUB_PLAY)
	    || static_cast<unsigned>(settings.startAction.mode) > static_cast<unsigned>(ActionMode::Synced)
	    || static_cast<unsigned>(settings.stopAction.mode) > static_cast<unsigned>(ActionMode::Synced)
	    || static_cast<unsigned>(settings.startAction.interval) > static_cast<unsigned>(Interval::ThirtySecondTriplet)
	    || static_cast<unsigned>(settings.stopAction.interval) > static_cast<unsigned>(Interval::ThirtySecondTriplet))
		return false;
	for (const auto& track : settings.tracks)
		if (static_cast<unsigned>(track.source) > static_cast<unsigned>(Source::Balanced)
		    || static_cast<unsigned>(track.monitoring) > static_cast<unsigned>(Monitoring::On)
		    || static_cast<unsigned>(track.playback) > static_cast<unsigned>(PlaybackMode::OneShot)
		    || static_cast<unsigned>(track.colour) > static_cast<unsigned>(Colour::White)
		    || track.transposeCents < -2450 || track.transposeCents > 2450)
			return false;
	return true;
}

bool validManifest(const Manifest& manifest) {
	if (manifest.phraseCount > kTrackCount || (manifest.phraseCount && !manifest.generation))
		return false;
	for (unsigned i = 0; i < manifest.phraseCount; ++i) {
		const auto& phrase = manifest.phrases[i];
		if (!validPhrase(phrase))
			return false;
		for (unsigned j = 0; j < i; ++j)
			if (manifest.phrases[j].slot == phrase.slot || manifest.phrases[j].identity == phrase.identity
			    || std::strncmp(manifest.phrases[j].path.data(), phrase.path.data(), phrase.path.size()) == 0)
				return false;
	}
	return true;
}
} // namespace

Status Persistence::decode(const SettingsDocument& document, Settings& settings, Manifest& manifest) {
	settings = defaultSettings();
	manifest = {};
	if (document.version != kSchemaVersion)
		return Status::SourceUnavailable;

	if (document.recordAction >= 0 && document.recordAction <= int(RecordAction::REC_DUB_PLAY))
		settings.recordAction = static_cast<RecordAction>(document.recordAction);
	validAction(document.startAction, settings.startAction);
	validAction(document.stopAction, settings.stopAction);

	if (document.trackCount > kPersistenceDocumentRows || document.phraseCount > kPersistenceDocumentRows)
		return Status::Invalid;
	if (document.phraseCount && !document.manifestGeneration)
		return Status::Invalid;
	bool duplicated[kTrackCount]{};
	for (unsigned i = 0; i < document.trackCount; ++i) {
		const auto id = document.tracks[i].id;
		if (id < 0 || id >= kTrackCount)
			continue;
		for (unsigned j = i + 1; j < document.trackCount; ++j)
			if (document.tracks[j].id == id)
				duplicated[id] = true;
	}
	for (unsigned i = 0; i < document.trackCount; ++i) {
		const auto& row = document.tracks[i];
		if (row.id < 0 || row.id >= kTrackCount || duplicated[row.id])
			continue;
		auto& track = settings.tracks[row.id];
		if (enumValue(row.source, int(Source::Balanced)))
			track.source = static_cast<Source>(row.source);
		if (enumValue(row.monitoring, int(Monitoring::On)))
			track.monitoring = static_cast<Monitoring>(row.monitoring);
		if (row.recordDurationBars >= 0 && row.recordDurationBars <= UINT16_MAX)
			track.recordDurationBars = static_cast<uint16_t>(row.recordDurationBars);
		if (enumValue(row.playback, int(PlaybackMode::OneShot)))
			track.playback = static_cast<PlaybackMode>(row.playback);
		if (enumValue(row.colour, int(Colour::White)))
			track.colour = static_cast<Colour>(row.colour);
		if (row.transposeCents >= -2450 && row.transposeCents <= 2450)
			track.transposeCents = static_cast<int16_t>(row.transposeCents);
	}

	manifest.generation = document.manifestGeneration;
	bool duplicateSlot[kTrackCount]{};
	for (unsigned i = 0; i < document.phraseCount; ++i) {
		const auto& phrase = document.phrases[i];
		if (!validPhrase(phrase))
			continue;
		for (unsigned j = i + 1; j < document.phraseCount; ++j) {
			const auto& other = document.phrases[j];
			if (phrase.slot == other.slot || phrase.identity == other.identity
			    || std::strncmp(phrase.path.data(), other.path.data(), phrase.path.size()) == 0) {
				duplicateSlot[phrase.slot] = true;
				if (validSlot(other.slot))
					duplicateSlot[other.slot] = true;
			}
		}
	}
	for (unsigned i = 0; i < document.phraseCount; ++i) {
		const auto& phrase = document.phrases[i];
		if (!validPhrase(phrase) || duplicateSlot[phrase.slot] || manifest.phraseCount >= kTrackCount)
			continue;
		manifest.phrases[manifest.phraseCount++] = phrase;
	}
	return Status::Ok;
}

Status Persistence::encode(const Settings& settings, const Manifest& manifest, SettingsDocument& document) {
	document = SettingsDocument{};
	if (!validSettings(settings) || !validManifest(manifest))
		return Status::Invalid;
	document.version = kSchemaVersion;
	document.recordAction = static_cast<int32_t>(settings.recordAction);
	document.startAction = {static_cast<int32_t>(settings.startAction.mode),
	                        static_cast<int32_t>(settings.startAction.interval)};
	document.stopAction = {static_cast<int32_t>(settings.stopAction.mode),
	                       static_cast<int32_t>(settings.stopAction.interval)};
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		const auto& track = settings.tracks[slot];
		document.tracks[slot] = {slot,
		                         static_cast<int32_t>(track.source),
		                         static_cast<int32_t>(track.monitoring),
		                         track.recordDurationBars,
		                         static_cast<int32_t>(track.playback),
		                         static_cast<int32_t>(track.colour),
		                         track.transposeCents};
	}
	document.trackCount = kTrackCount;
	document.manifestGeneration = manifest.generation;
	for (unsigned i = 0; i < manifest.phraseCount; ++i) {
		const auto& phrase = manifest.phrases[i];
		document.phrases[i] = phrase;
	}
	document.phraseCount = manifest.phraseCount;
	return Status::Ok;
}

Status Persistence::admitLoadedPhrases(std::span<const SavedPhrase> phrases, MemoryProfile profile,
                                       Admission& admission) {
	admission = {};
	if (phrases.size() > kTrackCount || !activeAudioBytes(profile) || activeAudioBytes(profile) % kPageBytes)
		return Status::Invalid;
	const auto availablePages = static_cast<uint32_t>(activeAudioBytes(profile) / kPageBytes);
	bool duplicateSlot[kTrackCount]{};
	for (size_t i = 0; i < phrases.size(); ++i) {
		if (!validPhrase(phrases[i]))
			continue;
		for (size_t j = i + 1; j < phrases.size(); ++j) {
			const auto& other = phrases[j];
			if (!validPhrase(other))
				continue;
			if (phrases[i].slot == other.slot || phrases[i].identity == other.identity
			    || std::strncmp(phrases[i].path.data(), other.path.data(), phrases[i].path.size()) == 0) {
				duplicateSlot[phrases[i].slot] = true;
				duplicateSlot[other.slot] = true;
			}
		}
	}
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		const SavedPhrase* phrase = nullptr;
		for (const auto& candidate : phrases)
			if (candidate.slot == slot && validPhrase(candidate))
				phrase = &candidate;
		if (!phrase || duplicateSlot[slot])
			continue;
		const auto bytes = static_cast<uint64_t>(phrase->frames) * sizeof(Frame);
		const auto pages = (bytes + kPageBytes - 1) / kPageBytes;
		if (pages > availablePages - admission.usedPages)
			continue;
		admission.admitted[slot] = true;
		admission.phrases[slot] = *phrase;
		admission.usedPages += static_cast<uint32_t>(pages);
		++admission.count;
	}
	admission.omittedCount = static_cast<uint8_t>(phrases.size() - admission.count);
	return Status::Ok;
}

Status PersistenceState::installLoaded(const Settings& settings, const Manifest& manifest) {
	if (!validSettings(settings) || !validManifest(manifest))
		return Status::Invalid;
	liveSettings_ = committedSettings_ = pendingSettings_ = settings;
	committedManifest_ = pendingManifest_ = manifest;
	loadAdmission_ = {};
	liveSongGeneration_ = committedSongGeneration_ = pendingSongGeneration_ = 0;
	expectedPhraseCount_ = 0;
	archiveRequestMask_ = 0;
	snapshotRevisions_ = {};
	committedAudioRevisions_ = {};
	committedAudioRevisionMask_ = {};
	representedRevisionMask_ = {};
	pendingActive_ = false;
	replaceManifest_ = false;
	retainedStale_ = false;
	saveState_ = SaveState::Clean;
	return Status::Ok;
}

Status PersistenceState::prepareLoadAdmission(MemoryProfile profile) {
	return Persistence::admitLoadedPhrases(
	    std::span<const SavedPhrase>(committedManifest_.phrases).first(committedManifest_.phraseCount), profile,
	    loadAdmission_);
}

void PersistenceState::setLiveSettings(const Settings& settings, Generation songGeneration) {
	if (songGeneration < liveSongGeneration_)
		return;
	liveSettings_ = settings;
	liveSongGeneration_ = songGeneration;
	if (!pendingActive_ && saveState_ != SaveState::Failed && songGeneration != committedSongGeneration_)
		saveState_ = SaveState::Unsaved;
}

Status PersistenceState::beginPendingSave(Generation songGeneration, Generation manifestGeneration,
                                          uint8_t expectedPhraseCount, bool replaceManifest) {
	if (pendingActive_ || !songGeneration || songGeneration < liveSongGeneration_ || expectedPhraseCount > kTrackCount)
		return Status::Invalid;
	if (replaceManifest) {
		if (!manifestGeneration || manifestGeneration <= committedManifest_.generation)
			return Status::Invalid;
	}
	else if (expectedPhraseCount || manifestGeneration != committedManifest_.generation)
		return Status::Invalid;

	pendingSongGeneration_ = liveSongGeneration_ = songGeneration;
	pendingSettings_ = liveSettings_;
	expectedPhraseCount_ = replaceManifest ? expectedPhraseCount : committedManifest_.phraseCount;
	pendingManifest_ = replaceManifest ? Manifest{} : committedManifest_;
	pendingManifest_.generation = manifestGeneration;
	pendingActive_ = true;
	replaceManifest_ = replaceManifest;
	archiveRequestMask_ = 0;
	snapshotRevisions_ = {};
	representedRevisionMask_ = {};
	retainedStale_ = false;
	saveState_ = SaveState::Pending;
	return Status::Ok;
}

Status PersistenceState::requestSave(SongModel* model) {
	if (pendingActive_)
		return Status::Busy;
	const auto nextSongGeneration = std::max(liveSongGeneration_, committedSongGeneration_);
	if (nextSongGeneration == std::numeric_limits<Generation>::max())
		return Status::Unavailable;
	const auto songGeneration = nextSongGeneration + 1;
	const Settings snapshot = model && model->ready() ? model->engine().settings() : liveSettings_;
	setLiveSettings(snapshot, songGeneration);

	std::array<SavedPhrase, kTrackCount> retained{};
	std::array<bool, kTrackCount> hasRetained{};
	std::array<bool, kTrackCount> representedRevisions{};
	uint8_t retainedCount = 0;
	uint8_t requestMask = 0;
	bool staleRetained = false;
	if (model && model->ready()) {
		for (SlotId slot = 0; slot < kTrackCount; ++slot) {
			const auto state = model->engine().state(slot);
			const SavedPhrase* old = nullptr;
			for (unsigned i = 0; i < committedManifest_.phraseCount; ++i)
				if (committedManifest_.phrases[i].slot == slot)
					old = &committedManifest_.phrases[i];
			if (state.hasPhrase && !model->provisional(slot)) {
				requestMask |= uint8_t(1u << slot);
				representedRevisions[slot] = true;
			}
			else if (old
			         && (model->explicitlyUnbound(slot)
			             || (!state.hasPhrase && model->clip(slot) && !model->checkpointRestorePending(slot)))) {
				// A bound but empty loop slot is an intentional clear of its checkpoint.
				representedRevisions[slot] = true;
			}
			else if (old) {
				retained[slot] = *old;
				hasRetained[slot] = true;
				++retainedCount;
				staleRetained |= state.hasPhrase;
			}
			else if (state.hasPhrase) {
				// A provisional addition without a committed checkpoint remains unsaved.
				staleRetained = true;
			}
			else if (!old)
				representedRevisions[slot] = true;
		}
	}
	else {
		retainedCount = committedManifest_.phraseCount;
	}
	const auto requestedCount = static_cast<uint8_t>(std::popcount(requestMask));
	const bool replaceManifest = requestedCount != 0 || retainedCount != committedManifest_.phraseCount;
	const auto manifestGeneration = replaceManifest ? committedManifest_.generation + 1 : committedManifest_.generation;
	if (replaceManifest && manifestGeneration == 0)
		return Status::Unavailable;
	const auto expected = replaceManifest ? static_cast<uint8_t>(retainedCount + requestedCount) : 0;
	const auto status = beginPendingSave(songGeneration, manifestGeneration, expected, replaceManifest);
	if (status != Status::Ok)
		return status;
	// beginPendingSave resets its per-transaction state. Install the represented
	// revision mask only after that reset so retained stale references never gain
	// a baseline from the current in-memory phrase.
	representedRevisionMask_ = representedRevisions;
	retainedStale_ = staleRetained;
	if (replaceManifest) {
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			if (hasRetained[slot] && stageFinalized(retained[slot]) != Status::Ok) {
				failPending();
				return Status::Invalid;
			}
	}
	if (model && model->ready()) {
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			snapshotRevisions_[slot] = model->contentRevision(slot);
		for (SlotId slot = 0; slot < kTrackCount; ++slot) {
			if (!(requestMask & (1u << slot)))
				continue;
			const auto requestStatus = model->requestArchive(slot);
			if (requestStatus != Status::Ok) {
				for (SlotId cancelSlot = 0; cancelSlot < kTrackCount; ++cancelSlot)
					if (archiveRequestMask_ & (1u << cancelSlot))
						model->archive().cancel(cancelSlot);
				failPending();
				return requestStatus;
			}
			archiveRequestMask_ |= uint8_t(1u << slot);
		}
	}
	return Status::Ok;
}

void PersistenceState::serviceArchiveCandidates(SongModel& model) {
	if (!pendingActive_ || !archiveRequestMask_)
		return;
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		const auto bit = uint8_t(1u << slot);
		if (!(archiveRequestMask_ & bit))
			continue;
		const auto state = model.archive().state(slot);
		if (state == ArchiveState::Pending || state == ArchiveState::Writing)
			continue;
		if (state != ArchiveState::ReadyForPublication) {
			for (SlotId cancelSlot = 0; cancelSlot < kTrackCount; ++cancelSlot)
				if (archiveRequestMask_ & (1u << cancelSlot))
					model.archive().cancel(cancelSlot);
			failPending();
			return;
		}
		ArchiveCandidate candidate{};
		if (!model.archive().take(slot, candidate)) {
			for (SlotId cancelSlot = 0; cancelSlot < kTrackCount; ++cancelSlot)
				if (archiveRequestMask_ & (1u << cancelSlot))
					model.archive().cancel(cancelSlot);
			failPending();
			return;
		}
		SavedPhrase phrase{};
		phrase.slot = candidate.slot;
		phrase.identity = candidate.identity;
		phrase.frames = candidate.frames;
		phrase.path = candidate.path;
		if (stageFinalized(phrase) != Status::Ok) {
			for (SlotId cancelSlot = 0; cancelSlot < kTrackCount; ++cancelSlot)
				if (archiveRequestMask_ & (1u << cancelSlot))
					model.archive().cancel(cancelSlot);
			failPending();
			return;
		}
		archiveRequestMask_ &= static_cast<uint8_t>(~bit);
	}
}

Status PersistenceState::stageFinalized(const SavedPhrase& phrase) {
	if (!pendingActive_ || !replaceManifest_ || expectedPhraseCount_ == 0
	    || pendingManifest_.phraseCount >= expectedPhraseCount_)
		return Status::Invalid;
	if (!validPhrase(phrase))
		return Status::Invalid;
	for (unsigned i = 0; i < pendingManifest_.phraseCount; ++i) {
		const auto& staged = pendingManifest_.phrases[i];
		if (staged.slot == phrase.slot || staged.identity == phrase.identity
		    || std::strncmp(staged.path.data(), phrase.path.data(), phrase.path.size()) == 0)
			return Status::Invalid;
	}
	pendingManifest_.phrases[pendingManifest_.phraseCount++] = phrase;
	return Status::Ok;
}

bool PersistenceState::pendingReady() const {
	return pendingActive_ && archiveRequestMask_ == 0
	       && (!replaceManifest_ || pendingManifest_.phraseCount == expectedPhraseCount_);
}

Status PersistenceState::encodeForSave(SettingsDocument& document) const {
	if (pendingActive_) {
		if (!pendingReady())
			return Status::Busy;
		return Persistence::encode(pendingSettings_, pendingManifest_, document);
	}
	return Persistence::encode(committedSettings_, committedManifest_, document);
}

Status PersistenceState::encodeForSaveScratch() const {
	return encodeForSave(scratchDocument_);
}

Status PersistenceState::commitPendingAfterDocumentVerified() {
	return commitPendingAfterDocumentVerified(snapshotRevisions_);
}

Status
PersistenceState::commitPendingAfterDocumentVerified(const std::array<Generation, kTrackCount>& currentRevisions) {
	if (!pendingReady())
		return Status::Busy;
	committedSettings_ = pendingSettings_;
	committedManifest_ = pendingManifest_;
	committedSongGeneration_ = pendingSongGeneration_;
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		if (representedRevisionMask_[slot]) {
			committedAudioRevisions_[slot] = snapshotRevisions_[slot];
			committedAudioRevisionMask_[slot] = true;
		}
	pendingActive_ = false;
	replaceManifest_ = false;
	expectedPhraseCount_ = 0;
	bool audioChanged = retainedStale_;
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		audioChanged |= snapshotRevisions_[slot] != currentRevisions[slot];
	saveState_ =
	    !audioChanged && liveSongGeneration_ == committedSongGeneration_ ? SaveState::Clean : SaveState::Unsaved;
	archiveRequestMask_ = 0;
	retainedStale_ = false;
	representedRevisionMask_ = {};
	return Status::Ok;
}

void PersistenceState::cancelPending(SongModel* model) {
	if (model) {
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			if (archiveRequestMask_ & (1u << slot))
				model->archive().cancel(slot);
	}
	cancelPending();
}

void PersistenceState::failPending(SongModel* model) {
	if (model) {
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			if (archiveRequestMask_ & (1u << slot))
				model->archive().cancel(slot);
	}
	failPending();
}

void PersistenceState::cancelPending() {
	if (!pendingActive_)
		return;
	pendingActive_ = false;
	replaceManifest_ = false;
	expectedPhraseCount_ = 0;
	pendingManifest_ = {};
	archiveRequestMask_ = 0;
	retainedStale_ = false;
	saveState_ = liveSongGeneration_ == committedSongGeneration_ ? SaveState::Clean : SaveState::Unsaved;
}

void PersistenceState::failPending() {
	if (!pendingActive_)
		return;
	pendingActive_ = false;
	replaceManifest_ = false;
	expectedPhraseCount_ = 0;
	pendingManifest_ = {};
	archiveRequestMask_ = 0;
	retainedStale_ = false;
	saveState_ = SaveState::Failed;
}

} // namespace loop_station
