#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/persistence.h"
#include "deluge/loop_station/persistence_firmware.h"
#include "storage/storage_manager.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <variant>
#include <vector>

TEST_GROUP(LoopPersistence){};

namespace {
using namespace loop_station;

SettingsDocument validDocument() {
	SettingsDocument document{};
	document.version = Persistence::kSchemaVersion;
	document.recordAction = int(RecordAction::REC_DUB_PLAY);
	document.startAction = {int(ActionMode::Immediate), int(Interval::BarTriplet)};
	document.stopAction = {int(ActionMode::Synced), int(Interval::ThirtySecondTriplet)};
	document.trackCount = kTrackCount;
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		document.tracks[slot] = {slot,
		                         int(Source::Stereo),
		                         int(Monitoring::Off),
		                         uint16_t(12 + slot),
		                         int(loop_station::PlaybackMode::OneShot),
		                         int(Colour::Pink),
		                         int16_t(-2450 + slot)};
	}
	return document;
}

SavedPhrase savedPhrase(SlotId slot, Generation identity, FrameIndex frames, const char* path) {
	SavedPhrase phrase{};
	phrase.slot = slot;
	phrase.identity = identity;
	phrase.frames = frames;
	std::strncpy(phrase.path.data(), path, phrase.path.size() - 1);
	return phrase;
}

struct Field {
	std::string name;
	std::variant<int32_t, std::string> value;
};
class RecordingSerializer final : public Serializer {
public:
	std::vector<Field> fields;
	void writeAttribute(const char* name, int32_t value, bool = true) override { fields.push_back({name, value}); }
	void writeAttribute(const char* name, const char* value, bool = true) override { fields.push_back({name, value}); }
	void writeAttributeHex(const char*, int32_t, int32_t, bool = true) override {}
	void writeAttributeHexBytes(const char*, uint8_t*, int32_t, bool = true) override {}
	void writeTagNameAndSeperator(const char*) override {}
	void writeTag(const char*, int32_t, bool = false) override {}
	void writeTag(const char*, const char*, bool = false, bool = true) override {}
	void writeOpeningTag(const char*, bool = true, bool = false) override {}
	void writeOpeningTagBeginning(const char*, bool = false, bool = true) override {}
	void writeOpeningTagEnd(bool = true) override {}
	void closeTag(bool = false) override {}
	void writeClosingTag(const char*, bool = true, bool = false) override {}
	void writeArrayStart(const char*, bool = true, bool = false) override {}
	void writeArrayEnding(const char*, bool = true, bool = false) override {}
	void printIndents() override {}
	void insertCommaIfNeeded() override {}
	void write(const char*) override {}
	Error closeFileAfterWriting(const char* = nullptr, const char* = nullptr, const char* = nullptr) override {
		return Error::NONE;
	}
	void reset() override { fields.clear(); }
};
class RecordingDeserializer final : public Deserializer {
public:
	explicit RecordingDeserializer(std::vector<Field>& fields) : fields(fields) {}
	std::vector<Field>& fields;
	size_t index = 0;
	bool objectOpen = true;
	bool prepareToReadTagOrAttributeValueOneCharAtATime() override { return false; }
	char readNextCharOfTagOrAttributeValue() override { return 0; }
	int32_t getNumCharsRemainingInValueBeforeEndOfCluster() override { return 0; }
	const char* readNextTagOrAttributeName() override {
		return index < fields.size() ? fields[index].name.c_str() : "";
	}
	const char* readTagOrAttributeValue() override {
		if (index >= fields.size())
			return "";
		if (auto* text = std::get_if<std::string>(&fields[index].value))
			return text->c_str();
		return "";
	}
	int32_t readTagOrAttributeValueInt() override {
		return index < fields.size() && std::holds_alternative<int32_t>(fields[index].value)
		           ? std::get<int32_t>(fields[index].value)
		           : 0;
	}
	int32_t readTagOrAttributeValueHex(int32_t) override { return 0; }
	int readTagOrAttributeValueHexBytes(uint8_t*, int32_t) override { return 0; }
	Error tryReadingFirmwareTagFromFile(const char*, bool = false) override { return Error::NONE; }
	const char* readNextCharsOfTagOrAttributeValue(int32_t) override { return ""; }
	Error readTagOrAttributeValueString(String*) override { return Error::NONE; }
	bool match(char ch) override { return ch == '{' && std::exchange(objectOpen, false); }
	void exitTag(const char* name = nullptr, bool = false) override {
		if (name)
			++index;
	}
	void reset() override {
		index = 0;
		objectOpen = true;
	}
};
} // namespace

TEST(LoopPersistence, defaultsAndStableIdsRoundTripIncludingImmediateMode) {
	SettingsDocument document = validDocument();
	Settings settings{};
	Manifest manifest{};
	CHECK(Status::Ok == Persistence::decode(document, settings, manifest));
	CHECK_EQUAL(int(RecordAction::REC_DUB_PLAY), int(settings.recordAction));
	CHECK_EQUAL(int(ActionMode::Immediate), int(settings.startAction.mode));
	CHECK_EQUAL(int(Interval::BarTriplet), int(settings.startAction.interval));
	CHECK_EQUAL(int(ActionMode::Synced), int(settings.stopAction.mode));
	CHECK_EQUAL(int(Interval::ThirtySecondTriplet), int(settings.stopAction.interval));
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		CHECK_EQUAL(int(Source::Stereo), int(settings.tracks[slot].source));
		CHECK_EQUAL(int(Monitoring::Off), int(settings.tracks[slot].monitoring));
		CHECK_EQUAL(12 + slot, settings.tracks[slot].recordDurationBars);
		CHECK_EQUAL(int(loop_station::PlaybackMode::OneShot), int(settings.tracks[slot].playback));
		CHECK_EQUAL(int(Colour::Pink), int(settings.tracks[slot].colour));
		CHECK_EQUAL(-2450 + slot, settings.tracks[slot].transposeCents);
	}

	SettingsDocument encoded{};
	CHECK(Status::Ok == Persistence::encode(settings, manifest, encoded));
	CHECK_EQUAL(Persistence::kSchemaVersion, encoded.version);
	CHECK_EQUAL(0, encoded.startAction.mode); // Immediate is a valid persistent value.
	CHECK_EQUAL(int(Interval::BarTriplet), encoded.startAction.interval);
}

TEST(LoopPersistence, CustomWhiteAndDuplicateColoursKeepTheirSlots) {
	auto settings = defaultSettings();
	Manifest manifest{};
	manifest.generation = 42;
	manifest.phraseCount = kTrackCount;
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		auto& track = settings.tracks[slot];
		track.source = static_cast<Source>(slot % 4);
		track.monitoring = static_cast<Monitoring>(slot % 2);
		track.recordDurationBars = 10 + slot;
		track.playback = static_cast<loop_station::PlaybackMode>(slot % 2);
		track.transposeCents = -1234 + slot * 200;
		char path[16]{};
		std::snprintf(path, sizeof(path), "L%1X000000.WAV", unsigned(slot + 1));
		manifest.phrases[slot] = savedPhrase(slot, 1000 + slot, 44100 + slot, path);
	}
	settings.tracks[0].colour = settings.tracks[5].colour = Colour::White;
	settings.tracks[2].colour = settings.tracks[7].colour = Colour::Sky;
	SettingsDocument document{};
	CHECK(Status::Ok == Persistence::encode(settings, manifest, document));
	CHECK_EQUAL(0, document.tracks[0].id);
	CHECK_EQUAL(5, document.tracks[5].id);
	CHECK_EQUAL(int(Colour::White), document.tracks[0].colour);
	CHECK_EQUAL(int(Colour::White), document.tracks[5].colour);
	std::swap(document.tracks[0], document.tracks[5]); // Logical IDs, not row positions, own the colour.
	Settings restored{};
	Manifest restoredManifest{};
	CHECK(Status::Ok == Persistence::decode(document, restored, restoredManifest));
	CHECK_EQUAL(manifest.generation, restoredManifest.generation);
	CHECK_EQUAL(manifest.phraseCount, restoredManifest.phraseCount);
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		const auto& expected = settings.tracks[slot];
		const auto& actual = restored.tracks[slot];
		CHECK_EQUAL(int(expected.source), int(actual.source));
		CHECK_EQUAL(int(expected.monitoring), int(actual.monitoring));
		CHECK_EQUAL(expected.recordDurationBars, actual.recordDurationBars);
		CHECK_EQUAL(int(expected.playback), int(actual.playback));
		CHECK_EQUAL(int(expected.colour), int(actual.colour));
		CHECK_EQUAL(expected.transposeCents, actual.transposeCents);
		const auto& phrase = restoredManifest.phrases[slot];
		CHECK_EQUAL(slot, phrase.slot);
		CHECK_EQUAL(1000 + slot, phrase.identity);
		CHECK_EQUAL(44100 + slot, phrase.frames);
		STRCMP_EQUAL(manifest.phrases[slot].path.data(), phrase.path.data());
	}
}

TEST(LoopPersistence, invalidValuesAndUnknownLegacyFieldsUseSafeDefaults) {
	auto document = validDocument();
	document.recordAction = 99;
	document.startAction.mode = 7;
	document.startAction.interval = 255;
	document.stopAction.mode = 0; // Valid Immediate must remain Immediate.
	document.tracks[0].source = 255;
	document.tracks[0].monitoring = -1;
	document.tracks[0].playback = 44;
	document.tracks[0].colour = 999;
	document.tracks[0].transposeCents = 3000;
	document.legacyRecordMode = 1;
	document.legacyImmediateStop = 1;
	Settings settings{};
	Manifest manifest{};
	CHECK(Status::Ok == Persistence::decode(document, settings, manifest));
	const auto defaults = defaultSettings();
	CHECK_EQUAL(int(defaults.recordAction), int(settings.recordAction));
	CHECK_EQUAL(int(defaults.startAction.mode), int(settings.startAction.mode));
	CHECK_EQUAL(int(defaults.startAction.interval), int(settings.startAction.interval));
	CHECK_EQUAL(int(ActionMode::Immediate), int(settings.stopAction.mode));
	CHECK_EQUAL(int(defaults.tracks[0].source), int(settings.tracks[0].source));
	CHECK_EQUAL(int(defaults.tracks[0].monitoring), int(settings.tracks[0].monitoring));
	CHECK_EQUAL(int(defaults.tracks[0].playback), int(settings.tracks[0].playback));
	CHECK_EQUAL(int(defaults.tracks[0].colour), int(settings.tracks[0].colour));
	CHECK_EQUAL(defaults.tracks[0].transposeCents, settings.tracks[0].transposeCents);
}

TEST(LoopPersistence, unsupportedSchemaAndDuplicateTrackIdsDoNotOverwriteDefaults) {
	auto document = validDocument();
	document.version = Persistence::kSchemaVersion + 1;
	Settings settings{};
	Manifest manifest{};
	CHECK(Status::SourceUnavailable == Persistence::decode(document, settings, manifest));
	CHECK_EQUAL(int(defaultSettings().recordAction), int(settings.recordAction));
	CHECK_EQUAL(int(defaultSettings().tracks[0].colour), int(settings.tracks[0].colour));

	document = validDocument();
	document.tracks[1].id = 0; // Duplicate slot zero; neither row may choose a winner.
	CHECK(Status::Ok == Persistence::decode(document, settings, manifest));
	CHECK_EQUAL(int(defaultTrackSettings(0).source), int(settings.tracks[0].source));
	CHECK_EQUAL(int(defaultTrackSettings(0).colour), int(settings.tracks[0].colour));
	CHECK_EQUAL(int(Source::Stereo), int(settings.tracks[2].source));
}

TEST(LoopPersistence, duplicateOrMalformedPhraseReferencesAreNotPublished) {
	auto document = validDocument();
	document.manifestGeneration = 7;
	document.phraseCount = 2;
	document.phrases[0] = savedPhrase(0, 101, 44100, "L1000000.WAV");
	document.phrases[1] = savedPhrase(0, 102, 88200, "L1000001.WAV");
	Settings settings{};
	Manifest manifest{};
	CHECK(Status::Ok == Persistence::decode(document, settings, manifest));
	CHECK_EQUAL(7, manifest.generation);
	CHECK_EQUAL(0, manifest.phraseCount);
	CHECK_EQUAL(0, manifest.phrases[0].frames);

	document.phraseCount = 1;
	document.phrases[0] = savedPhrase(0, 103, 44100, "../BAD.WAV");
	CHECK(Status::Ok == Persistence::decode(document, settings, manifest));
	CHECK_EQUAL(0, manifest.phraseCount);
}

TEST(LoopPersistence, phraseReferencesRequireAManifestGeneration) {
	auto document = validDocument();
	document.phraseCount = 1;
	document.phrases[0] = savedPhrase(0, 104, 44100, "L1000000.WAV");
	Settings settings{};
	Manifest manifest{};
	CHECK(Status::Invalid == Persistence::decode(document, settings, manifest));
	CHECK_EQUAL(0, manifest.phraseCount);

	manifest.generation = 0;
	manifest.phraseCount = 1;
	manifest.phrases[0] = savedPhrase(0, 104, 44100, "L1000000.WAV");
	SettingsDocument encoded{};
	CHECK(Status::Invalid == Persistence::encode(defaultSettings(), manifest, encoded));
}

TEST(LoopPersistence, encodeRejectsCastInvalidSettingsEnumsAndTranspose) {
	auto settings = defaultSettings();
	Manifest manifest{};
	SettingsDocument document{};
	settings.recordAction = static_cast<RecordAction>(99);
	CHECK(Status::Invalid == Persistence::encode(settings, manifest, document));

	settings = defaultSettings();
	settings.startAction.mode = static_cast<ActionMode>(9);
	CHECK(Status::Invalid == Persistence::encode(settings, manifest, document));

	settings = defaultSettings();
	settings.tracks[2].transposeCents = 3000;
	CHECK(Status::Invalid == Persistence::encode(settings, manifest, document));
}

TEST(LoopPersistence, directAdmissionRejectsDuplicateIdentityOrPathAcrossSlots) {
	SavedPhrase phrases[2] = {savedPhrase(0, 201, 1000, "L1000000.WAV"), savedPhrase(1, 202, 1000, "L2000000.WAV")};
	Admission admission{};
	phrases[1].identity = phrases[0].identity;
	CHECK(Status::Ok == Persistence::admitLoadedPhrases(phrases, MemoryProfile::Preferred, admission));
	CHECK_EQUAL(0, admission.count);

	phrases[1].identity = 202;
	std::strncpy(phrases[1].path.data(), phrases[0].path.data(), phrases[1].path.size() - 1);
	CHECK(Status::Ok == Persistence::admitLoadedPhrases(phrases, MemoryProfile::Preferred, admission));
	CHECK_EQUAL(1, admission.count); // The mismatched slot/path reference is malformed and omitted.
	CHECK(admission.admitted[0]);
	CHECK_FALSE(admission.admitted[1]);
}

TEST(LoopPersistence, tenSecondAdmissionNeverTruncatesAWholePhrase) {
	constexpr FrameIndex tenSeconds = kTenSecondFrames;
	SavedPhrase phrases[kTrackCount]{};
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		char path[16]{};
		std::snprintf(path, sizeof(path), "L%1X000000.WAV", unsigned(slot + 1));
		phrases[slot] = savedPhrase(slot, Generation(slot + 1), tenSeconds, path);
	}
	phrases[kTrackCount - 1].frames = tenSeconds + 345; // Crosses the next page-rounded boundary.

	Admission admission{};
	CHECK(Status::Ok == Persistence::admitLoadedPhrases(phrases, MemoryProfile::TenSecond, admission));
	CHECK_EQUAL(7, admission.count);
	CHECK(admission.admitted[0]);
	CHECK(admission.admitted[1]);
	for (SlotId slot = 0; slot < kTrackCount - 1; ++slot)
		CHECK_EQUAL(tenSeconds, admission.phrases[slot].frames);
	CHECK_FALSE(admission.admitted[kTrackCount - 1]);
}

TEST(LoopPersistence, stateKeepsCommittedManifestUntilEveryCandidateIsFinalized) {
	PersistenceState state;
	Settings initial = defaultSettings();
	Manifest oldManifest{};
	oldManifest.generation = 4;
	oldManifest.phraseCount = 1;
	oldManifest.phrases[0] = savedPhrase(0, 401, 44100, "L1000000.WAV");
	CHECK(Status::Ok == state.installLoaded(initial, oldManifest));
	CHECK(Status::Ok == state.beginPendingSave(10, 5, 2, true));
	CHECK_EQUAL(int(SaveState::Pending), int(state.saveState()));
	CHECK(Status::Ok == state.stageFinalized(savedPhrase(1, 402, 22050, "L2000000.WAV")));
	CHECK_FALSE(state.pendingReady());
	SettingsDocument document{};
	CHECK(Status::Busy == state.encodeForSave(document));
	CHECK_EQUAL(4, state.committedManifest().generation);
	CHECK_EQUAL(1, state.committedManifest().phraseCount);
	CHECK(Status::Ok == state.stageFinalized(savedPhrase(2, 403, 22050, "L3000000.WAV")));
	CHECK(state.pendingReady());
	CHECK(Status::Ok == state.encodeForSave(document));
	CHECK_EQUAL(5, document.manifestGeneration);
	CHECK_EQUAL(2, document.phraseCount);
	CHECK_EQUAL(4, state.committedManifest().generation);
	CHECK(Status::Ok == state.commitPendingAfterDocumentVerified());
	CHECK_EQUAL(5, state.committedManifest().generation);
	CHECK_EQUAL(2, state.committedManifest().phraseCount);
	CHECK_EQUAL(int(SaveState::Clean), int(state.saveState()));
}

TEST(LoopPersistence, simulatedRestartUsesOldManifestUntilVerifiedCommitThenNewManifest) {
	PersistenceState state;
	Manifest oldManifest{};
	oldManifest.generation = 12;
	oldManifest.phraseCount = 1;
	oldManifest.phrases[0] = savedPhrase(0, 1201, 44100, "L1000000.WAV");
	CHECK(Status::Ok == state.installLoaded(defaultSettings(), oldManifest));

	SettingsDocument durableSong{};
	CHECK(Status::Ok == state.encodeForSave(durableSong));
	Settings restartedSettings{};
	Manifest restartedManifest{};
	CHECK(Status::Ok == Persistence::decode(durableSong, restartedSettings, restartedManifest));
	CHECK_EQUAL(12, restartedManifest.generation);
	CHECK_EQUAL(1201, restartedManifest.phrases[0].identity);

	CHECK(Status::Ok == state.beginPendingSave(1, 13, 1, true));
	CHECK(Status::Ok == state.stageFinalized(savedPhrase(0, 1301, 22050, "L1000001.WAV")));
	SettingsDocument replacementSong{};
	CHECK(Status::Ok == state.encodeForSave(replacementSong));
	// Until the replacement Song document is verified and committed, a restart
	// still reads the previously durable complete manifest.
	CHECK(Status::Ok == Persistence::decode(durableSong, restartedSettings, restartedManifest));
	CHECK_EQUAL(12, restartedManifest.generation);
	CHECK_EQUAL(1201, restartedManifest.phrases[0].identity);

	CHECK(Status::Ok == state.commitPendingAfterDocumentVerified());
	CHECK(Status::Ok == state.encodeForSave(replacementSong));
	CHECK(Status::Ok == Persistence::decode(replacementSong, restartedSettings, restartedManifest));
	CHECK_EQUAL(13, restartedManifest.generation);
	CHECK_EQUAL(1301, restartedManifest.phrases[0].identity);
}

TEST(LoopPersistence, stateRejectsNonmonotonicAndDuplicateFinalizedCandidates) {
	PersistenceState state;
	Manifest manifest{};
	manifest.generation = 8;
	CHECK(Status::Ok == state.installLoaded(defaultSettings(), manifest));
	CHECK(Status::Invalid == state.beginPendingSave(2, 8, 1, true));
	CHECK(Status::Ok == state.beginPendingSave(2, 9, 2, true));
	const auto first = savedPhrase(0, 501, 100, "L1000000.WAV");
	CHECK(Status::Ok == state.stageFinalized(first));
	CHECK(Status::Invalid == state.stageFinalized(first));
	CHECK(Status::Invalid == state.stageFinalized(savedPhrase(1, 501, 100, "L2000000.WAV")));
	CHECK(Status::Invalid == state.stageFinalized(savedPhrase(1, 502, 100, "L1000000.WAV")));
	CHECK_EQUAL(1, state.pendingManifest().phraseCount);
}

TEST(LoopPersistence, emptyManifestCanCommitAndCancelledOrFailedSaveKeepsCommittedReferences) {
	PersistenceState emptyState;
	CHECK(Status::Ok == emptyState.installLoaded(defaultSettings(), Manifest{}));
	CHECK(Status::Ok == emptyState.beginPendingSave(1, 0, 0, false));
	CHECK(emptyState.pendingReady());
	CHECK(Status::Ok == emptyState.commitPendingAfterDocumentVerified());
	CHECK_EQUAL(0, emptyState.committedManifest().phraseCount);
	CHECK_EQUAL(int(SaveState::Clean), int(emptyState.saveState()));

	PersistenceState clearedState;
	Manifest existing{};
	existing.generation = 3;
	existing.phraseCount = 1;
	existing.phrases[0] = savedPhrase(0, 590, 100, "L1000000.WAV");
	CHECK(Status::Ok == clearedState.installLoaded(defaultSettings(), existing));
	CHECK(Status::Ok == clearedState.beginPendingSave(1, 4, 0, true));
	CHECK(clearedState.pendingReady());
	SettingsDocument emptyDocument{};
	CHECK(Status::Ok == clearedState.encodeForSave(emptyDocument));
	CHECK_EQUAL(4, emptyDocument.manifestGeneration);
	CHECK_EQUAL(0, emptyDocument.phraseCount);
	CHECK(Status::Ok == clearedState.commitPendingAfterDocumentVerified());
	CHECK_EQUAL(0, clearedState.committedManifest().phraseCount);

	PersistenceState state;
	Manifest manifest{};
	manifest.generation = 3;
	manifest.phraseCount = 1;
	manifest.phrases[0] = savedPhrase(0, 601, 100, "L1000000.WAV");
	CHECK(Status::Ok == state.installLoaded(defaultSettings(), manifest));
	CHECK(Status::Ok == state.beginPendingSave(1, 3, 0, false)); // Preserve the exact committed manifest.
	CHECK(state.pendingReady());
	CHECK(Status::Ok == state.commitPendingAfterDocumentVerified());
	CHECK_EQUAL(1, state.committedManifest().phraseCount);
	CHECK_EQUAL(3, state.committedManifest().generation);

	CHECK(Status::Ok == state.beginPendingSave(2, 4, 2, true));
	CHECK(Status::Ok == state.stageFinalized(manifest.phrases[0]));
	CHECK(Status::Ok == state.stageFinalized(savedPhrase(1, 602, 100, "L2000000.WAV")));
	state.cancelPending();
	CHECK_EQUAL(3, state.committedManifest().generation);
	CHECK_EQUAL(1, state.committedManifest().phraseCount);
	CHECK_EQUAL(int(SaveState::Unsaved), int(state.saveState()));

	CHECK(Status::Ok == state.beginPendingSave(2, 4, 2, true));
	CHECK(Status::Ok == state.stageFinalized(manifest.phrases[0]));
	CHECK(Status::Ok == state.stageFinalized(savedPhrase(1, 602, 100, "L2000000.WAV")));
	state.failPending();
	CHECK_EQUAL(3, state.committedManifest().generation);
	CHECK_EQUAL(1, state.committedManifest().phraseCount);
	CHECK_EQUAL(int(SaveState::Failed), int(state.saveState()));
}

TEST(LoopPersistence, statePreservesUnsavedEditsThatOccurAfterTheSaveSnapshot) {
	PersistenceState state;
	CHECK(Status::Ok == state.installLoaded(defaultSettings(), Manifest{}));
	CHECK(Status::Ok == state.beginPendingSave(11, 0, 0, false));
	Settings laterEdit = defaultSettings();
	laterEdit.tracks[0].transposeCents = 1200;
	state.setLiveSettings(laterEdit, 12);
	CHECK(Status::Ok == state.commitPendingAfterDocumentVerified());
	CHECK_EQUAL(int(SaveState::Unsaved), int(state.saveState()));
	CHECK_EQUAL(0, state.committedSettings().tracks[0].transposeCents);
	CHECK_EQUAL(1200, state.liveSettings().tracks[0].transposeCents);
}

TEST(LoopPersistence, retainedManifestReferencesDoNotAdvanceLiveAudioRevisionBaseline) {
	PersistenceState state;
	Manifest manifest{};
	manifest.generation = 5;
	manifest.phraseCount = 1;
	manifest.phrases[0] = savedPhrase(0, 501, 44100, "L1000000.WAV");
	CHECK(Status::Ok == state.installLoaded(defaultSettings(), manifest));
	CHECK(Status::Ok == state.beginPendingSave(1, 5, 0, false));
	CHECK(state.pendingReady());
	CHECK(Status::Ok == state.commitPendingAfterDocumentVerified());
	CHECK_EQUAL(501, state.committedManifest().phrases[0].identity);
	CHECK_FALSE(state.hasCommittedAudioRevision(0));
}

TEST(LoopPersistence, firmwareSerializerAdapterRoundTripsSettingsAnd64BitManifestReferences) {
	PersistenceState source;
	Settings settings = defaultSettings();
	settings.recordAction = RecordAction::REC_DUB_PLAY;
	settings.startAction = {ActionMode::Immediate, Interval::BarTriplet};
	settings.tracks[0].playback = loop_station::PlaybackMode::OneShot;
	Manifest manifest{};
	manifest.generation = 0x12345678abcdef01ull;
	manifest.phraseCount = 1;
	manifest.phrases[0] = savedPhrase(0, 0x8877665544332211ull, 44100, "L1000000.WAV");
	CHECK(Status::Ok == source.installLoaded(settings, manifest));
	RecordingSerializer writer;
	CHECK(Status::Ok == writeLoopStationSettings(writer, source));
	CHECK(writer.fields.size() > 20);

	PersistenceState restored;
	RecordingDeserializer reader(writer.fields);
	CHECK(Status::Ok == readLoopStationSettings(reader, restored));
	CHECK_EQUAL(int(RecordAction::REC_DUB_PLAY), int(restored.committedSettings().recordAction));
	CHECK_EQUAL(int(ActionMode::Immediate), int(restored.committedSettings().startAction.mode));
	CHECK_EQUAL(int(loop_station::PlaybackMode::OneShot), int(restored.committedSettings().tracks[0].playback));
	CHECK_EQUAL(manifest.generation, restored.committedManifest().generation);
	CHECK_EQUAL(manifest.phrases[0].identity, restored.committedManifest().phrases[0].identity);
	STRCMP_EQUAL("L1000000.WAV", restored.committedManifest().phrases[0].path.data());
}

TEST(LoopPersistence, firmwareDeserializerRejectsWrappedPhraseSlotAndNegativeFrames) {
	PersistenceState source;
	Manifest manifest{};
	manifest.generation = 4;
	manifest.phraseCount = 1;
	manifest.phrases[0] = savedPhrase(0, 701, 44100, "L1000000.WAV");
	CHECK(Status::Ok == source.installLoaded(defaultSettings(), manifest));
	RecordingSerializer writer;
	CHECK(Status::Ok == writeLoopStationSettings(writer, source));
	for (auto& field : writer.fields) {
		if (field.name == "phrase0Slot")
			field.value = int32_t(256);
		if (field.name == "phrase0Frames")
			field.value = int32_t(-1);
	}
	PersistenceState restored;
	RecordingDeserializer reader(writer.fields);
	CHECK(Status::Ok == readLoopStationSettings(reader, restored));
	CHECK_EQUAL(0, restored.committedManifest().phraseCount);
}
