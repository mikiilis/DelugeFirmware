#include "loop_station/revision.h"
#include "model/clip/audio_clip.h"
#include "model/sample/sample.h"
#include "storage/audio/audio_file_manager.h"
#include <algorithm>
#include <cstring>
#include <utility>

LoopStationRevision::LoopStationRevision(LoopStationRevision const& other)
    : filePath(other.filePath), startPos(other.startPos), endPos(other.endPos), loopLength(other.loopLength),
      originalLength(other.originalLength), sampleZeroTicks(other.sampleZeroTicks),
      sampleZeroFrames(other.sampleZeroFrames) {
	setSample(other.sample);
}

LoopStationRevision& LoopStationRevision::operator=(LoopStationRevision const& other) {
	if (this != &other) {
		filePath = other.filePath;
		startPos = other.startPos;
		endPos = other.endPos;
		loopLength = other.loopLength;
		originalLength = other.originalLength;
		sampleZeroTicks = other.sampleZeroTicks;
		sampleZeroFrames = other.sampleZeroFrames;
		setSample(other.sample);
	}
	return *this;
}

LoopStationRevision::LoopStationRevision(LoopStationRevision&& other) noexcept
    : filePath(other.filePath), sample(other.sample), startPos(other.startPos), endPos(other.endPos),
      loopLength(other.loopLength), originalLength(other.originalLength), sampleZeroTicks(other.sampleZeroTicks),
      sampleZeroFrames(other.sampleZeroFrames) {
	other.filePath.clear();
	other.sample = nullptr;
}

LoopStationRevision& LoopStationRevision::operator=(LoopStationRevision&& other) noexcept {
	if (this != &other) {
		setSample(nullptr);
		filePath = other.filePath;
		other.filePath.clear();
		sample = other.sample;
		other.sample = nullptr;
		startPos = other.startPos;
		endPos = other.endPos;
		loopLength = other.loopLength;
		originalLength = other.originalLength;
		sampleZeroTicks = other.sampleZeroTicks;
		sampleZeroFrames = other.sampleZeroFrames;
	}
	return *this;
}

LoopStationRevision::~LoopStationRevision() {
	setSample(nullptr);
}

void LoopStationRevision::setSample(Sample* newSample) {
	if (sample == newSample) {
		return;
	}
	if (sample) {
		sample->removeReason("LSRH");
	}
	sample = newSample;
	if (sample) {
		sample->addReason();
	}
}

void LoopStationRevision::capture(AudioClip const& clip) {
	filePath.set(clip.sampleHolder.audioFile ? &clip.sampleHolder.audioFile->filePath : &clip.sampleHolder.filePath);
	setSample(static_cast<Sample*>(clip.sampleHolder.audioFile));
	startPos = clip.sampleHolder.startPos;
	endPos = clip.sampleHolder.endPos;
	loopLength = std::max<int32_t>(1, clip.loopLength);
	originalLength = std::max<int32_t>(1, clip.originalLength);
	sampleZeroTicks = 0;
	sampleZeroFrames = clip.loopStationPhaseZeroFrames();
}

void LoopStationRevision::apply(AudioClip& clip) const {
	clip.unassignVoiceSample(true);
	clip.sampleHolder.setAudioFile(sample);
	clip.sampleHolder.filePath.set(sample ? &sample->filePath : &filePath);
	clip.sampleHolder.startPos = startPos;
	clip.sampleHolder.endPos = endPos;
	clip.loopLength = loopLength;
	clip.originalLength = originalLength;
	clip.loopStationSampleZeroTicks = sampleZeroTicks;
	clip.loopStationSampleZeroFrames = sampleZeroFrames;
	clip.renderData.xScroll = -1;
}

Error LoopStationRevision::loadSample(bool mayActuallyReadFile) {
	if (sample || filePath.isEmpty()) {
		return Error::NONE;
	}
	Error error = Error::NONE;
	AudioFile* loaded = audioFileManager.getAudioFileFromFilename(filePath, mayActuallyReadFile, &error, nullptr,
	                                                              AudioFileType::SAMPLE);
	if (loaded) {
		setSample(static_cast<Sample*>(loaded));
	}
	return error;
}

bool LoopStationRevisionHistory::hasCurrent() const {
	return cursor >= 0 && cursor < static_cast<int32_t>(revisions.size());
}

bool LoopStationRevisionHistory::canUndo() const {
	return hasCurrent();
}

bool LoopStationRevisionHistory::canRedo() const {
	return cursor + 1 < static_cast<int32_t>(revisions.size());
}

LoopStationRevision const* LoopStationRevisionHistory::current() const {
	return hasCurrent() ? &revisions[cursor] : nullptr;
}

void LoopStationRevisionHistory::clear() {
	provisional = {};
	provisionalPending = false;
	revisions.clear();
	cursor = -1;
}

bool LoopStationRevisionHistory::seedFromClip(AudioClip& clip) {
	if (!revisions.empty() || !clip.sampleHolder.audioFile) {
		return !revisions.empty();
	}
	try {
		revisions.emplace_back();
	} catch (deluge::exception error) {
		if (error == deluge::exception::BAD_ALLOC) {
			return false;
		}
		throw;
	}
	revisions.back().capture(clip);
	cursor = 0;
	return true;
}

bool LoopStationRevisionHistory::prepareAppend(AudioClip& clip, bool preserveCurrent, size_t pending) {
	if (provisionalPending) {
		return false;
	}
	// If this is the first overdub of a legacy/current clip, reserve both its
	// seed revision and the provisional revision which will follow it.
	size_t required = static_cast<size_t>(std::max<int32_t>(1, cursor + 2));
	if (preserveCurrent && revisions.empty()) {
		required = 2;
	}
	required += pending;
	try {
		revisions.reserve(required);
	} catch (deluge::exception error) {
		if (error == deluge::exception::BAD_ALLOC) {
			return false;
		}
		throw;
	}
	return !preserveCurrent || seedFromClip(clip);
}

bool LoopStationRevisionHistory::publishGeneration(LoopStationRevision&& revision) {
	if (!revision.sample || revisions.capacity() < static_cast<size_t>(cursor + 2))
		return false;
	revision.filePath.set(&revision.sample->filePath);
	auto firstRedo = revisions.begin() + std::min<size_t>(revisions.size(), static_cast<size_t>(cursor + 1));
	revisions.erase(firstRedo, revisions.end());
	revisions.emplace_back(std::move(revision));
	cursor = static_cast<int32_t>(revisions.size()) - 1;
	return true;
}

void LoopStationRevisionHistory::appendCurrent(AudioClip& clip) {
	// Do not destroy a redo branch until a new revision has actually
	// committed. This method is called only after the new Sample is valid.
	auto firstRedo = revisions.begin() + std::min<size_t>(revisions.size(), static_cast<size_t>(cursor + 1));
	revisions.erase(firstRedo, revisions.end());
	revisions.emplace_back();
	revisions.back().capture(clip);
	cursor = static_cast<int32_t>(revisions.size()) - 1;
}

bool LoopStationRevisionHistory::stageCurrent(AudioClip& clip) {
	if (provisionalPending || !clip.sampleHolder.audioFile) {
		return false;
	}
	provisional.capture(clip);
	provisionalPending = provisional.sample != nullptr;
	return provisionalPending;
}

bool LoopStationRevisionHistory::publishStaged(AudioClip& clip) {
	if (!provisionalPending || !provisional.sample) {
		return false;
	}
	// The recorder creates the final path asynchronously, after stageCurrent().
	// Refresh it only once finalization has succeeded.
	provisional.filePath.set(&provisional.sample->filePath);
	if (clip.sampleHolder.audioFile != provisional.sample)
		provisional.apply(clip);
	auto firstRedo = revisions.begin() + std::min<size_t>(revisions.size(), static_cast<size_t>(cursor + 1));
	revisions.erase(firstRedo, revisions.end());
	revisions.emplace_back(std::move(provisional));
	provisional = {};
	provisionalPending = false;
	cursor = static_cast<int32_t>(revisions.size()) - 1;
	return true;
}

void LoopStationRevisionHistory::discardStaged() {
	provisional = {};
	provisionalPending = false;
}

Error LoopStationRevisionHistory::restoreCurrent(AudioClip& clip) {
	LoopStationRevision* revision = hasCurrent() ? &revisions[cursor] : nullptr;
	if (revision) {
		Error error = revision->loadSample(true);
		if (error != Error::NONE || !revision->sample) {
			return error == Error::NONE ? Error::FILE_NOT_FOUND : error;
		}
		revision->apply(clip);
	}
	else {
		clip.unassignVoiceSample(true);
		clip.sampleHolder.setAudioFile(nullptr);
		clip.sampleHolder.filePath.clear();
		clip.sampleHolder.startPos = 0;
		clip.sampleHolder.endPos = 0;
		clip.loopStationSampleZeroTicks = 0;
		clip.loopStationSampleZeroFrames = 0;
		clip.renderData.xScroll = -1;
	}
	return Error::NONE;
}

Error LoopStationRevisionHistory::rejectStaged(AudioClip& clip) {
	Error error = restoreCurrent(clip);
	if (error != Error::NONE) {
		// Even an unavailable durable WAV must not leave playback holding the rejected Sample.
		// Keep the durable history entry so a later retry can reload it.
		clip.unassignVoiceSample(true);
		clip.sampleHolder.setAudioFile(nullptr);
		clip.sampleHolder.filePath.clear();
		clip.sampleHolder.startPos = 0;
		clip.sampleHolder.endPos = 0;
		clip.loopStationSampleZeroTicks = 0;
		clip.loopStationSampleZeroFrames = 0;
		clip.renderData.xScroll = -1;
	}
	discardStaged();
	return error;
}

Error LoopStationRevisionHistory::loadSamples(AudioClip* currentClip, bool mayActuallyReadFiles) {
	Error firstError = Error::NONE;
	for (auto& revision : revisions) {
		if (currentClip && currentClip->sampleHolder.audioFile
		    && !strcmp(revision.filePath.get(), currentClip->sampleHolder.audioFile->filePath.get())) {
			revision.setSample(static_cast<Sample*>(currentClip->sampleHolder.audioFile));
			continue;
		}
		Error error = revision.loadSample(mayActuallyReadFiles);
		if (error != Error::NONE && firstError == Error::NONE) {
			firstError = error;
		}
	}
	return firstError;
}

Error LoopStationRevisionHistory::navigate(AudioClip& clip, bool redo) {
	int32_t target = cursor + (redo ? 1 : -1);
	if (target < -1 || target >= static_cast<int32_t>(revisions.size())) {
		return Error::RESULT_TAG_UNUSED;
	}

	LoopStationRevision* revision = target >= 0 ? &revisions[target] : nullptr;
	if (revision) {
		Error error = revision->loadSample(true);
		if (error != Error::NONE || !revision->sample) {
			return error == Error::NONE ? Error::FILE_NOT_FOUND : error;
		}
	}

	if (revision) {
		revision->apply(clip);
	}
	else {
		clip.unassignVoiceSample(true);
		clip.sampleHolder.setAudioFile(nullptr);
		clip.sampleHolder.filePath.clear();
		clip.sampleHolder.startPos = 0;
		clip.sampleHolder.endPos = 0;
		clip.loopStationSampleZeroTicks = 0;
		clip.loopStationSampleZeroFrames = 0;
		if (!revisions.empty()) {
			clip.loopLength = revisions.front().loopLength;
			clip.originalLength = revisions.front().originalLength;
		}
	}
	clip.renderData.xScroll = -1;
	cursor = target;
	return Error::NONE;
}
