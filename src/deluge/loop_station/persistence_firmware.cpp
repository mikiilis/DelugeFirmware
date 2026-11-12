#include "persistence_firmware.h"

#include "storage/storage_manager.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace loop_station {
namespace {
int32_t signedWord(uint32_t word) {
	return std::bit_cast<int32_t>(word);
}

void makeName(char* output, const char* prefix, const char* suffix) {
	std::strcpy(output, prefix);
	std::strcat(output, suffix);
}

void makeIndexedName(char* output, const char* prefix, unsigned index, const char* suffix) {
	std::strcpy(output, prefix);
	const auto length = std::strlen(output);
	output[length] = static_cast<char>('0' + index);
	output[length + 1] = '\0';
	std::strcat(output, suffix);
}

void writeGeneration(Serializer& writer, const char* prefix, Generation value) {
	char name[40];
	makeName(name, prefix, "High");
	writer.writeAttribute(name, signedWord(static_cast<uint32_t>(value >> 32)));
	makeName(name, prefix, "Low");
	writer.writeAttribute(name, signedWord(static_cast<uint32_t>(value)));
}

void writeRow(Serializer& writer, unsigned index, const PersistedTrack& row) {
	char name[36];
	makeIndexedName(name, "track", index, "Id");
	writer.writeAttribute(name, row.id);
	makeIndexedName(name, "track", index, "Source");
	writer.writeAttribute(name, row.source);
	makeIndexedName(name, "track", index, "Monitoring");
	writer.writeAttribute(name, row.monitoring);
	makeIndexedName(name, "track", index, "RecordDurationBars");
	writer.writeAttribute(name, row.recordDurationBars);
	makeIndexedName(name, "track", index, "Playback");
	writer.writeAttribute(name, row.playback);
	makeIndexedName(name, "track", index, "Colour");
	writer.writeAttribute(name, row.colour);
	makeIndexedName(name, "track", index, "TransposeCents");
	writer.writeAttribute(name, row.transposeCents);
}

void writePhrase(Serializer& writer, unsigned index, const SavedPhrase& phrase) {
	char name[36];
	makeIndexedName(name, "phrase", index, "Slot");
	writer.writeAttribute(name, phrase.slot);
	makeIndexedName(name, "phrase", index, "IdentityHigh");
	writer.writeAttribute(name, signedWord(static_cast<uint32_t>(phrase.identity >> 32)));
	makeIndexedName(name, "phrase", index, "IdentityLow");
	writer.writeAttribute(name, signedWord(static_cast<uint32_t>(phrase.identity)));
	makeIndexedName(name, "phrase", index, "Frames");
	writer.writeAttribute(name, static_cast<int32_t>(phrase.frames));
	makeIndexedName(name, "phrase", index, "Path");
	writer.writeAttribute(name, phrase.path.data());
}

void exitField(Deserializer& reader, const char* name) {
	reader.exitTag(name);
}

bool readTrackField(Deserializer& reader, const char* name, SettingsDocument& document) {
	char field[40];
	for (unsigned i = 0; i < kPersistenceDocumentRows; ++i) {
		auto& row = document.tracks[i];
#define TRACK_FIELD(suffix, member)                                                                                    \
	makeIndexedName(field, "track", i, suffix);                                                                        \
	if (!std::strcmp(name, field)) {                                                                                   \
		row.member = reader.readTagOrAttributeValueInt();                                                              \
		exitField(reader, name);                                                                                       \
		document.trackCount = std::max<uint8_t>(document.trackCount, static_cast<uint8_t>(i + 1));                     \
		return true;                                                                                                   \
	}
		TRACK_FIELD("Id", id)
		TRACK_FIELD("Source", source)
		TRACK_FIELD("Monitoring", monitoring)
		TRACK_FIELD("RecordDurationBars", recordDurationBars)
		TRACK_FIELD("Playback", playback)
		TRACK_FIELD("Colour", colour)
		TRACK_FIELD("TransposeCents", transposeCents)
#undef TRACK_FIELD
	}
	return false;
}

bool readPhraseField(Deserializer& reader, const char* name, SettingsDocument& document, uint32_t (&high)[8],
                     uint32_t (&low)[8]) {
	char field[40];
	for (unsigned i = 0; i < kPersistenceDocumentRows; ++i) {
		auto& phrase = document.phrases[i];
		makeIndexedName(field, "phrase", i, "Slot");
		if (!std::strcmp(name, field)) {
			const auto value = reader.readTagOrAttributeValueInt();
			phrase.slot = value >= 0 && value < kTrackCount ? static_cast<SlotId>(value) : kInvalidSlot;
			exitField(reader, name);
			document.phraseCount = std::max<uint8_t>(document.phraseCount, static_cast<uint8_t>(i + 1));
			return true;
		}
		makeIndexedName(field, "phrase", i, "Frames");
		if (!std::strcmp(name, field)) {
			const auto value = reader.readTagOrAttributeValueInt();
			phrase.frames = value > 0 ? static_cast<FrameIndex>(value) : 0;
			exitField(reader, name);
			document.phraseCount = std::max<uint8_t>(document.phraseCount, static_cast<uint8_t>(i + 1));
			return true;
		}
		makeIndexedName(field, "phrase", i, "IdentityHigh");
		if (!std::strcmp(name, field)) {
			high[i] = static_cast<uint32_t>(reader.readTagOrAttributeValueInt());
			exitField(reader, name);
			document.phraseCount = std::max<uint8_t>(document.phraseCount, static_cast<uint8_t>(i + 1));
			return true;
		}
		makeIndexedName(field, "phrase", i, "IdentityLow");
		if (!std::strcmp(name, field)) {
			low[i] = static_cast<uint32_t>(reader.readTagOrAttributeValueInt());
			exitField(reader, name);
			document.phraseCount = std::max<uint8_t>(document.phraseCount, static_cast<uint8_t>(i + 1));
			return true;
		}
		makeIndexedName(field, "phrase", i, "Path");
		if (!std::strcmp(name, field)) {
			const char* path = reader.readTagOrAttributeValue();
			if (path)
				std::strncpy(phrase.path.data(), path, phrase.path.size() - 1);
			exitField(reader, name);
			document.phraseCount = std::max<uint8_t>(document.phraseCount, static_cast<uint8_t>(i + 1));
			return true;
		}
	}
	return false;
}
} // namespace

Status writeLoopStationSettings(Serializer& writer, const PersistenceState& state) {
	const auto status = state.encodeForSaveScratch();
	if (status != Status::Ok)
		return status;
	const auto& document = state.scratchDocument();
	writer.writeOpeningTagBeginning("loopStation", true);
	writer.writeAttribute("version", static_cast<int32_t>(document.version));
	writer.writeAttribute("recordAction", document.recordAction);
	writer.writeAttribute("startMode", document.startAction.mode);
	writer.writeAttribute("startInterval", document.startAction.interval);
	writer.writeAttribute("stopMode", document.stopAction.mode);
	writer.writeAttribute("stopInterval", document.stopAction.interval);
	writer.writeAttribute("trackCount", document.trackCount);
	writeGeneration(writer, "manifestGeneration", document.manifestGeneration);
	writer.writeAttribute("phraseCount", document.phraseCount);
	for (unsigned i = 0; i < document.trackCount; ++i)
		writeRow(writer, i, document.tracks[i]);
	for (unsigned i = 0; i < document.phraseCount; ++i)
		writePhrase(writer, i, document.phrases[i]);
	writer.writeOpeningTagEnd();
	writer.writeClosingTag("loopStation");
	return Status::Ok;
}

Status readLoopStationSettings(Deserializer& reader, PersistenceState& state) {
	auto& document = state.scratchDocument();
	document = SettingsDocument{};
	uint32_t manifestHigh = 0, manifestLow = 0;
	uint32_t phraseHigh[kTrackCount]{}, phraseLow[kTrackCount]{};
	if (!reader.match('{'))
		return Status::Invalid;
	const char* name;
	while (*(name = reader.readNextTagOrAttributeName())) {
		if (!std::strcmp(name, "version")) {
			document.version = static_cast<uint32_t>(reader.readTagOrAttributeValueInt());
			exitField(reader, name);
		}
		else if (!std::strcmp(name, "recordAction")) {
			document.recordAction = reader.readTagOrAttributeValueInt();
			exitField(reader, name);
		}
		else if (!std::strcmp(name, "startMode")) {
			document.startAction.mode = reader.readTagOrAttributeValueInt();
			exitField(reader, name);
		}
		else if (!std::strcmp(name, "startInterval")) {
			document.startAction.interval = reader.readTagOrAttributeValueInt();
			exitField(reader, name);
		}
		else if (!std::strcmp(name, "stopMode")) {
			document.stopAction.mode = reader.readTagOrAttributeValueInt();
			exitField(reader, name);
		}
		else if (!std::strcmp(name, "stopInterval")) {
			document.stopAction.interval = reader.readTagOrAttributeValueInt();
			exitField(reader, name);
		}
		else if (!std::strcmp(name, "trackCount")) {
			document.trackCount =
			    static_cast<uint8_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), 0, 255));
			exitField(reader, name);
		}
		else if (!std::strcmp(name, "manifestGenerationHigh")) {
			manifestHigh = static_cast<uint32_t>(reader.readTagOrAttributeValueInt());
			exitField(reader, name);
		}
		else if (!std::strcmp(name, "manifestGenerationLow")) {
			manifestLow = static_cast<uint32_t>(reader.readTagOrAttributeValueInt());
			exitField(reader, name);
		}
		else if (!std::strcmp(name, "phraseCount")) {
			document.phraseCount =
			    static_cast<uint8_t>(std::clamp<int32_t>(reader.readTagOrAttributeValueInt(), 0, 255));
			exitField(reader, name);
		}
		else if (!readTrackField(reader, name, document)
		         && !readPhraseField(reader, name, document, phraseHigh, phraseLow))
			reader.exitTag(name);
	}
	document.manifestGeneration = (Generation(manifestHigh) << 32) | manifestLow;
	for (unsigned i = 0; i < kTrackCount; ++i)
		document.phrases[i].identity = (Generation(phraseHigh[i]) << 32) | phraseLow[i];
	reader.exitTag(nullptr, true);
	Settings settings{};
	Manifest manifest{};
	const auto status = Persistence::decode(document, settings, manifest);
	const auto installStatus = state.installLoaded(settings, manifest);
	return installStatus == Status::Ok ? status : installStatus;
}
} // namespace loop_station
