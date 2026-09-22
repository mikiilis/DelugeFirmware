/* Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "loop_station/loop_station.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"
#include "storage/storage_manager.h"
#include <algorithm>
#include <bit>
#include <cstring>

extern "C" {
#include "fatfs/diskio.h"
LBA_t clst2sect(FATFS*, DWORD);
}

SampleRecorder::LoopCacheEntry* SampleRecorder::loopCacheAt(uint32_t index) {
	for (auto& entry : loopCache_)
		if (entry.cluster && entry.index == index)
			return &entry;
	return nullptr;
}

Error SampleRecorder::setupLoopOverdub(uint32_t frames, Sample* source, uint32_t sourceStart, uint32_t sourceFrame) {
	struct Guard {
		bool& flag;
		Guard(bool& value) : flag(value) { flag = true; }
		~Guard() { flag = false; }
	} guard(loopPreparing_);
	uint64_t bytes = static_cast<uint64_t>(frames) * 6 + sample->audioDataStartPosBytes;
	if (!frames || bytes > UINT32_MAX || recordingNumChannels != 2 || recordingExtraMargins)
		return Error::MAX_FILE_SIZE_REACHED;
	loopWriteSnapshot_ = delugeAlloc(Cluster::size);
	if (!loopWriteSnapshot_)
		return Error::INSUFFICIENT_RAM;
	auto count = (bytes + Cluster::size - 1) / Cluster::size;
	auto error = sample->clusters.insertSampleClustersAtEnd(count - 1);
	if (error != Error::NONE) {
		delugeDealloc(loopWriteSnapshot_);
		loopWriteSnapshot_ = nullptr;
		return error;
	}
	// Transfer the initial writer reason to the cache. All further reasons are
	// owned by cache entries, not the linear recorder's unwritten interval.
	loopSource_ = source;
	if (loopSource_)
		loopSource_->addReason();
	traceLifecycle(LifecycleEvent::SOURCE_RETAINED);
	loopSourceStart_ = sourceStart;
	loopSourceFrame_ = sourceFrame;
	loopFrames_ = frames;
	loopCache_[0] = {currentRecordCluster, 0, true, false};
	currentRecordCluster = nullptr;
	currentRecordClusterIndex = firstUnwrittenClusterIndex = 0;
	numSamplesToRunBeforeBeginningCapturing = 0;
	numSamplesExtraToCaptureAtEndSyncingWise = 0;
	totalSampleLengthNowKnown(frames, frames);
	return prepareLoopClusters();
}

Error SampleRecorder::prepareLoopClusters() {
	auto sourceError = prepareLoopSourceClusters();
	if (sourceError != Error::NONE)
		return sourceError;
	uint32_t count = sample->clusters.getNumElements();
	std::array<uint32_t, 6> wanted{};
	size_t wantedCount = 0;
	auto addWindow = [&](uint32_t frame, uint32_t aheadCount) {
		uint32_t current = (static_cast<uint64_t>(frame) * 6 + sample->audioDataStartPosBytes) / Cluster::size;
		for (uint32_t ahead = 0; ahead < std::min(aheadCount, count); ++ahead) {
			uint32_t index = (current + ahead) % count;
			if (std::find(wanted.begin(), wanted.begin() + wantedCount, index) == wanted.begin() + wantedCount)
				wanted[wantedCount++] = index;
		}
	};
	if (loopEnding_ && loopSource_) {
		// Playback continues on the audio clock while untouched backing copies
		// at card-service speed. Reserve both windows, with audible reads first.
		addWindow(loopPlaybackCursor_, 4);
		if (!loopOverdubReady())
			addWindow(loopFrameCursor_, 2);
	}
	else
		addWindow(loopFrameCursor_, 4);
	// At most six wanted entries leave room for dirty entries to drain within
	// the existing eight-entry cache. Background copying waits if they cannot.
	for (size_t i = 0; i < wantedCount; ++i) {
		uint32_t index = wanted[i];
		if (loopCacheAt(index))
			continue;
		LoopCacheEntry* target = nullptr;
		for (auto& entry : loopCache_) {
			bool needed =
			    entry.cluster
			    && std::find(wanted.begin(), wanted.begin() + wantedCount, entry.index) != wanted.begin() + wantedCount;
			if (!entry.cluster || (!entry.dirty && !entry.writing && !needed)) {
				target = &entry;
				break;
			}
		}
		if (!target)
			return Error::NONE; // Dirty entries will be serviced first.
		if (target->cluster) {
			target->cluster->numReasonsHeldBySampleRecorder--;
			audioFileManager.removeReasonFromCluster(*target->cluster, "LSCE");
			*target = {};
		}
		auto* descriptor = sample->clusters.getElement(index);
		bool existing = descriptor->sdAddress != 0;
		auto* cluster = descriptor->getCluster(sample, index, existing ? CLUSTER_ENQUEUE : CLUSTER_DONT_LOAD);
		if (!cluster)
			return Error::INSUFFICIENT_RAM;
		cluster->numReasonsHeldBySampleRecorder++;
		if (!existing) {
			std::memset(cluster->data, 0, Cluster::size + 7);
			cluster->loaded = true;
		}
		*target = {cluster, index, false, false};
	}
	return Error::NONE;
}

Error SampleRecorder::prepareLoopSourceClusters() {
	if (!loopSource_ || !loopFrames_)
		return Error::NONE;
	std::array<uint32_t, 4> wanted{};
	size_t wantedCount = 0;
	uint32_t bytesPerFrame = loopSource_->byteDepth * loopSource_->numChannels;
	if (!bytesPerFrame)
		return Error::FILE_CORRUPTED;
	// Walk bytes, including split frames and the wrap back to the source start.
	// Sampling frame offsets can alias to one position on a short loop and miss
	// the next cluster even when the very next audio buffer needs it.
	uint64_t first = loopSource_->audioDataStartPosBytes + uint64_t(loopSourceStart_) * bytesPerFrame;
	uint64_t end = first + uint64_t(loopFrames_) * bytesPerFrame;
	uint64_t address = first + uint64_t((loopSourceFrame_ + loopFrameCursor_) % loopFrames_) * bytesPerFrame;
	uint64_t remaining = end - first;
	while (remaining && wantedCount < wanted.size()) {
		uint32_t index = address / Cluster::size;
		if (std::find(wanted.begin(), wanted.begin() + wantedCount, index) == wanted.begin() + wantedCount)
			wanted[wantedCount++] = index;
		uint64_t count = std::min({remaining, end - address, uint64_t(Cluster::size - address % Cluster::size)});
		remaining -= count;
		address += count;
		if (address == end)
			address = first;
	}
	for (auto& entry : loopSourceCache_) {
		bool needed =
		    entry.cluster
		    && std::find(wanted.begin(), wanted.begin() + wantedCount, entry.index) != wanted.begin() + wantedCount;
		if (entry.cluster && !needed) {
			audioFileManager.removeReasonFromCluster(*entry.cluster, "LSSX");
			entry = {};
		}
	}
	for (size_t i = 0; i < wantedCount; ++i) {
		uint32_t index = wanted[i];
		if (std::any_of(loopSourceCache_.begin(), loopSourceCache_.end(),
		                [index](auto const& entry) { return entry.cluster && entry.index == index; }))
			continue;
		auto target = std::find_if(loopSourceCache_.begin(), loopSourceCache_.end(),
		                           [](auto const& entry) { return entry.cluster == nullptr; });
		if (target == loopSourceCache_.end() || index >= static_cast<uint32_t>(loopSource_->clusters.getNumElements()))
			return Error::SD_CARD;
		Cluster* cluster = loopSource_->clusters.getElement(index)->getCluster(loopSource_, index, CLUSTER_ENQUEUE);
		if (!cluster)
			return Error::INSUFFICIENT_RAM;
		*target = {cluster, index};
	}
	return Error::NONE;
}

Error SampleRecorder::readLoopSourceFrame(uint32_t frame, StereoSample& destination) {
	if (!loopSource_ || !loopSource_->byteDepth || !loopSource_->numChannels)
		return Error::FILE_CORRUPTED;
	uint32_t sourceFrame = loopSourceStart_ + (loopSourceFrame_ + frame) % loopFrames_;
	uint64_t address = loopSource_->audioDataStartPosBytes
	                   + static_cast<uint64_t>(sourceFrame) * loopSource_->byteDepth * loopSource_->numChannels;
	destination = {};
	for (uint32_t channel = 0; channel < loopSource_->numChannels; ++channel) {
		uint32_t value = 0;
		for (uint32_t byte = 0; byte < loopSource_->byteDepth; ++byte, ++address) {
			uint32_t index = address / Cluster::size;
			auto entry = std::find_if(loopSourceCache_.begin(), loopSourceCache_.end(), [index](auto const& candidate) {
				return candidate.cluster && candidate.index == index;
			});
			if (entry == loopSourceCache_.end() || !entry->cluster->loaded)
				return Error::SD_CARD;
			value |= static_cast<uint32_t>(static_cast<uint8_t>(entry->cluster->data[address % Cluster::size]))
			         << (8 * (4 - loopSource_->byteDepth + byte));
		}
		int32_t sampleValue = std::bit_cast<int32_t>(value);
		if (channel == 0)
			destination.l = destination.r = sampleValue;
		else if (channel == 1)
			destination.r = sampleValue;
	}
	return Error::NONE;
}

void SampleRecorder::processLoopOverdub(std::span<StereoSample> backing, std::span<const StereoSample> input,
                                        bool backgroundCopy) {
	using deluge::loop_station::kCaptureGainShift;
	using deluge::loop_station::restoreCaptureGain;
	using deluge::loop_station::saturatingAdd;
	if (loopPreparing_ || !loopFrames_ || hadCardError || terminalError != Error::NONE || captureOutputBackingMissing
	    || status >= RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING)
		return;
	if (loopEnding_ && loopSource_ && !backgroundCopy) {
		// Background copying owns the writer cursor after the input gate closes.
		// Audible playback keeps its own cursor, so new additions do not vanish
		// while the untouched tail or a preceding revision is still completing.
		for (auto& frame : backing) {
			if (loopPlaybackCursor_ < numSamplesCaptured) {
				uint64_t offset = sample->audioDataStartPosBytes + static_cast<uint64_t>(loopPlaybackCursor_) * 6;
				uint32_t values[2]{};
				for (uint32_t byte = 0; byte < 6; ++byte) {
					auto address = offset + byte;
					auto* entry = loopCacheAt(address / Cluster::size);
					if (!entry || !entry->cluster->loaded) {
						// Missing prefetched audio is an underrun, not proof of an SD write error.
						captureOutputBackingMissing = true;
						traceLifecycle(LifecycleEvent::CACHE_UNDERRUN, nullptr, address / Cluster::size);
						return;
					}
					values[byte / 3] |=
					    static_cast<uint32_t>(static_cast<uint8_t>(entry->cluster->data[address % Cluster::size]))
					    << (8 * (byte % 3 + 1));
				}
				frame = {std::bit_cast<int32_t>(values[0]) >> kCaptureGainShift,
				         std::bit_cast<int32_t>(values[1]) >> kCaptureGainShift};
			}
			loopPlaybackCursor_ = (loopPlaybackCursor_ + 1) % loopFrames_;
		}
		return;
	}
	for (size_t frame = 0; frame < backing.size(); ++frame) {
		uint64_t offset = sample->audioDataStartPosBytes + static_cast<uint64_t>(loopFrameCursor_) * 6;
		std::array<LoopCacheEntry*, 6> entries{};
		std::array<char*, 6> bytes{};
		for (uint32_t byte = 0; byte < 6; ++byte) {
			uint64_t address = offset + byte;
			entries[byte] = loopCacheAt(address / Cluster::size);
			if (!entries[byte] || !entries[byte]->cluster->loaded) {
				// Never replace a loop with missing backing or silently drop input.
				// Missing prefetched audio is an underrun, not proof of an SD write error.
				captureOutputBackingMissing = true;
				traceLifecycle(LifecycleEvent::CACHE_UNDERRUN, nullptr, address / Cluster::size);
				return;
			}
			bytes[byte] = entries[byte]->cluster->data + address % Cluster::size;
		}
		StereoSample previous{restoreCaptureGain(backing[frame].l), restoreCaptureGain(backing[frame].r)};
		if (loopSource_ && numSamplesCaptured < loopFrames_) {
			auto error = readLoopSourceFrame(loopFrameCursor_, previous);
			if (error != Error::NONE) {
				captureOutputBackingMissing = true;
				return;
			}
		}
		if (numSamplesCaptured >= loopFrames_) {
			uint32_t left = 0, right = 0;
			for (uint32_t byte = 0; byte < 3; ++byte) {
				left |= static_cast<uint32_t>(static_cast<uint8_t>(*bytes[byte])) << (8 * (byte + 1));
				right |= static_cast<uint32_t>(static_cast<uint8_t>(*bytes[byte + 3])) << (8 * (byte + 1));
			}
			previous = {std::bit_cast<int32_t>(left), std::bit_cast<int32_t>(right)};
		}
		// Always return the current dry backing, including the retained source
		// on pass one. Audible envelope/pitch are applied by AudioOutput afterwards.
		backing[frame] = {previous.l >> kCaptureGainShift, previous.r >> kCaptureGainShift};
		if (!loopOverdubReady()) {
			StereoSample added = !backgroundCopy && captureOutputInputActive ? input[frame] : StereoSample{};
			uint32_t left = static_cast<uint32_t>(saturatingAdd(previous.l, added.l));
			uint32_t right = static_cast<uint32_t>(saturatingAdd(previous.r, added.r));
			auto magnitude = [](int32_t value) { return value < 0 ? uint32_t(-int64_t(value)) : uint32_t(value); };
			inputPeak = std::max({inputPeak, magnitude(added.l), magnitude(added.r)});
			capturedPeak = std::max(
			    {capturedPeak, magnitude(std::bit_cast<int32_t>(left)), magnitude(std::bit_cast<int32_t>(right))});
			for (uint32_t byte = 0; byte < 3; ++byte) {
				*bytes[byte] = static_cast<char>(left >> (8 * (byte + 1)));
				*bytes[byte + 3] = static_cast<char>(right >> (8 * (byte + 1)));
			}
			for (auto* entry : entries)
				entry->dirty = true;
			// Keep interpolation overlap bytes coherent for future VoiceSample readers.
			for (uint32_t byte = 0; byte < 6; ++byte) {
				uint64_t address = offset + byte;
				uint32_t local = address % Cluster::size;
				if (local < 7 && address >= Cluster::size) {
					if (auto* previousEntry = loopCacheAt(address / Cluster::size - 1))
						previousEntry->cluster->data[Cluster::size + local] = *bytes[byte];
				}
			}
			numSamplesCaptured = std::min(loopFrames_, numSamplesCaptured + 1);
		}
		loopFrameCursor_ = (loopFrameCursor_ + 1) % loopFrames_;
		if (!backgroundCopy)
			loopPlaybackCursor_ = loopFrameCursor_;
	}
}

Error SampleRecorder::copyLoopRemainder() {
	// Copy only untouched frames, independently of audible playback/transport.
	// Work is bounded and uses asynchronous source-cluster loading.
	std::array<StereoSample, 128> backing{};
	uint32_t count = std::min<uint32_t>(backing.size(), loopFrames_ - numSamplesCaptured);
	if (!count)
		return Error::NONE;
	// No live input remains to lose: unavailable destination buffers are a
	// reason to retry after cache writes/loads, not a terminal backing error.
	uint64_t firstByte = sample->audioDataStartPosBytes + uint64_t(loopFrameCursor_) * 6;
	uint64_t lastByte = firstByte + uint64_t(count) * 6 - 1;
	for (uint32_t index = firstByte / Cluster::size; index <= lastByte / Cluster::size; ++index) {
		auto* entry = loopCacheAt(index);
		if (!entry || !entry->cluster->loaded)
			return Error::NONE;
	}

	Cluster* reading = nullptr;
	uint32_t readingIndex = UINT32_MAX;
	for (uint32_t frame = 0; frame < count; ++frame) {
		uint32_t sourceFrame = loopSourceStart_ + (loopSourceFrame_ + loopFrameCursor_ + frame) % loopFrames_;
		uint64_t address = loopSource_->audioDataStartPosBytes
		                   + static_cast<uint64_t>(sourceFrame) * loopSource_->byteDepth * loopSource_->numChannels;
		for (uint32_t channel = 0; channel < loopSource_->numChannels; ++channel) {
			uint32_t value = 0;
			for (uint32_t byte = 0; byte < loopSource_->byteDepth; ++byte, ++address) {
				uint32_t index = address / Cluster::size;
				if (readingIndex != index) {
					if (reading)
						audioFileManager.removeReasonFromCluster(*reading, "LSBR");
					reading = nullptr;
					if (index >= static_cast<uint32_t>(loopSource_->clusters.getNumElements()))
						return Error::SD_CARD;
					reading = loopSource_->clusters.getElement(index)->getCluster(loopSource_, index, CLUSTER_ENQUEUE);
					if (!reading)
						return Error::INSUFFICIENT_RAM;
					if (!reading->loaded) {
						audioFileManager.removeReasonFromCluster(*reading, "LSBL");
						return Error::NONE;
					}
					readingIndex = index;
				}
				value |= static_cast<uint32_t>(static_cast<uint8_t>(reading->data[address % Cluster::size]))
				         << (8 * (4 - loopSource_->byteDepth + byte));
			}
			if (channel == 0)
				backing[frame].l = backing[frame].r = std::bit_cast<int32_t>(value) >> 3;
			else
				backing[frame].r = std::bit_cast<int32_t>(value) >> 3;
		}
	}
	if (reading)
		audioFileManager.removeReasonFromCluster(*reading, "LSBR");
	processLoopOverdub(std::span(backing).first(count), {}, true);
	return terminalError;
}

Error SampleRecorder::serviceLoopWrites() {
	struct Guard {
		bool& flag;
		Guard(bool& value) : flag(value) { flag = true; }
		~Guard() { flag = false; }
	} guard(loopIOActive_);
	auto error = prepareLoopClusters();
	if (error != Error::NONE)
		return error;
	if (loopEnding_ && loopSource_ && !loopOverdubReady()) {
		error = copyLoopRemainder();
		if (error != Error::NONE)
			return error;
	}
	for (size_t offsetInCache = 0; offsetInCache < loopCache_.size(); ++offsetInCache) {
		size_t cacheIndex = (loopNextWriteEntry_ + offsetInCache) % loopCache_.size();
		auto& entry = loopCache_[cacheIndex];
		if (!entry.cluster || !entry.dirty || entry.writing)
			continue;
		loopNextWriteEntry_ = (cacheIndex + 1) % loopCache_.size();
		uint32_t index = entry.index;
		uint64_t offset = static_cast<uint64_t>(index) * Cluster::size;
		size_t size =
		    std::min<uint64_t>(Cluster::size, sample->audioDataStartPosBytes + sample->audioDataLengthBytes - offset);
		// Snapshot before yielding to SD. Audio can dirty the entry again during
		// the write; that newer version then stays pinned for a subsequent write.
		std::memcpy(loopWriteSnapshot_, entry.cluster->data, size);
		entry.dirty = false;
		entry.writing = true;
		auto failure =
		    deluge::loop_station::writeLoopSnapshot(*file, offset, {static_cast<std::byte*>(loopWriteSnapshot_), size},
		                                            [&] { return status == RecorderStatus::ABORTED; });
		entry.writing = false;
		if (status == RecorderStatus::ABORTED)
			return Error::NONE;
		if (failure.stage != deluge::loop_station::StorageStage::NONE) {
			entry.dirty = true;
			return latchStorageFailure(failure);
		}

		sample->clusters.getElement(index)->sdAddress = clst2sect(&fileSystem, file->inner().clust);
		return Error::NONE;
	}
	if (status == RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING) {
		auto closed = file->close();
		if (status == RecorderStatus::ABORTED)
			return Error::NONE;
		if (!closed)
			return latchStorageFailure(
			    {deluge::loop_station::StorageStage::CLOSE, static_cast<int32_t>(closed.error())});
		status = RecorderStatus::COMPLETE;
	}
	return Error::NONE;
}

void SampleRecorder::releaseLoopClusters() {
	for (auto& entry : loopSourceCache_) {
		if (entry.cluster) {
			audioFileManager.removeReasonFromCluster(*entry.cluster, "LSSR");
			entry = {};
		}
	}
	if (loopSource_) {
		loopSource_->removeReason("LSCB");
		loopSource_ = nullptr;
	}
	for (auto& entry : loopCache_) {
		if (entry.cluster) {
			entry.cluster->numReasonsHeldBySampleRecorder--;
			audioFileManager.removeReasonFromCluster(*entry.cluster, "LSCR");
			entry = {};
		}
	}
	if (loopWriteSnapshot_) {
		delugeDealloc(loopWriteSnapshot_);
		loopWriteSnapshot_ = nullptr;
	}
}

Error SampleRecorder::latchStorageFailure(deluge::loop_station::StorageFailure failure) {
	if (storageFailure.stage == deluge::loop_station::StorageStage::NONE)
		storageFailure = failure;
	Error error = Error::SD_CARD;
	using deluge::loop_station::StorageStage;
	if (failure.stage == StorageStage::SHORT_SEEK || failure.stage == StorageStage::SHORT_WRITE)
		error = Error::SD_CARD_FULL;
	else if (failure.rawResult == FR_WRITE_PROTECTED)
		error = Error::WRITE_PROTECTED;
	else if (failure.rawResult == FR_NOT_READY)
		error = Error::SD_CARD_NOT_PRESENT;
	else if (failure.rawResult == FR_NO_FILESYSTEM)
		error = Error::SD_CARD_NO_FILESYSTEM;
	if (terminalError == Error::NONE)
		terminalError = error;
	hadCardError = true;
	traceLifecycle(LifecycleEvent::STORAGE_FAILED, nullptr, failure.requested);
	return terminalError;
}
