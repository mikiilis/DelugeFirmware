/* Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "loop_station/loop_station.h"
#include "memory/general_memory_allocator.h"
#include "model/output.h"
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

Error SampleRecorder::setupLoopOverdub(uint32_t frames, Sample* source, uint32_t sourceStart, uint32_t sourceFrame,
                                       SampleRecorder* sourceRecorder) {
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
	if (source) {
		loopPendingFrames_ = static_cast<LoopPendingFrame*>(delugeAlloc(Cluster::size));
		if (!loopPendingFrames_)
			return Error::INSUFFICIENT_RAM;
		loopPendingCapacity_ = Cluster::size / sizeof(LoopPendingFrame);
	}
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
	loopSourceRecorder_ = sourceRecorder;
	if (loopSourceRecorder_) {
		loopSourceRecorder_->retainLoopReader();
		++loopSourceRecorder_->loopDependentReaders_;
	}
	if (loopSource_)
		loopSource_->addReason();
	traceLifecycle(LifecycleEvent::SOURCE_RETAINED);
	loopSourceStart_ = sourceStart;
	loopSourceFrame_ = sourceFrame;
	loopFrames_ = frames;
	loopFrameCursor_ = 0;
	loopPlaybackCursor_ = 0;
	loopAcceptedFrames_ = loopAppliedFrames_ = 0;
	loopAcceptedOriginFrame_ = 0;
	loopAcceptedOriginSet_ = false;
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
	if (loopSource_) {
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

SampleRecorder::LoopReadResult SampleRecorder::locateLoopSource(uint32_t frame, LoopSourceLocation& location) {
	SampleRecorder* owner = this;
	uint32_t contiguous = UINT32_MAX;
	for (uint32_t depth = 0; depth < 18; ++depth) {
		if (!owner->loopSource_ || !owner->loopFrames_)
			return LoopReadResult::INVALID;
		frame = (owner->loopSourceFrame_ + frame) % owner->loopFrames_;
		contiguous = std::min(contiguous, owner->loopFrames_ - frame);
		auto* parent = owner->loopSourceRecorder_;
		uint32_t prefix = parent ? (parent->loopSealed_ ? parent->loopSealedPrefix_ : parent->numSamplesCaptured) : 0;
		if (parent && parent->loopInputPendingAtFrame(frame))
			return LoopReadResult::PENDING;
		if (!parent || frame < prefix) {
			location = {
			    owner->loopSource_, owner->loopSourceStart_ + frame,
			    std::min(contiguous, (parent ? std::min(owner->loopFrames_, prefix) : owner->loopFrames_) - frame)};
			return LoopReadResult::READY;
		}
		if (parent->status == RecorderStatus::ABORTED || parent->captureOutputBackingMissing)
			return LoopReadResult::INVALID;
		if (!parent->isLoopOverdub())
			return LoopReadResult::PENDING; // Initial recording latency compensation.
		// A sealed take owns only its captured prefix. Its untouched tail is
		// still the retained source, even before a cumulative WAV exists.
		owner = parent;
	}
	return LoopReadResult::INVALID;
}

bool SampleRecorder::loopInputPendingAtFrame(uint32_t frame) const {
	if (!loopFrames_ || !loopAcceptedOriginSet_ || loopAppliedFrames_ >= loopAcceptedFrames_)
		return false;

	frame %= loopFrames_;
	uint64_t firstPending = loopAppliedFrames_;
	uint32_t firstPendingFrame = (loopAcceptedOriginFrame_ + firstPending % loopFrames_) % loopFrames_;
	uint64_t distance = (uint64_t(frame) + loopFrames_ - firstPendingFrame) % loopFrames_;
	return firstPending + distance < loopAcceptedFrames_;
}

Error SampleRecorder::prepareLoopSourceClusters() {
	if (!loopSource_ || !loopFrames_)
		return Error::NONE;
	if (numSamplesCaptured >= loopFrames_ && !loopDependentReaders_) {
		for (auto& entry : loopSourceCache_) {
			if (entry.cluster)
				audioFileManager.removeReasonFromCluster(*entry.cluster, "LSPF");
			entry = {};
		}
		if (loopSourceRecorder_) {
			--loopSourceRecorder_->loopDependentReaders_;
			loopSourceRecorder_->releaseLoopReader();
			loopSourceRecorder_ = nullptr;
		}
		loopSealedPrefix_ = loopFrames_;
		return Error::NONE;
	}
	struct Key {
		Sample* sample = nullptr;
		uint32_t index = 0;
	};
	std::array<Key, 4> wanted{};
	size_t wantedCount = 0;
	auto addWindow = [&](uint32_t frame, uint32_t remaining) -> Error {
		while (remaining && wantedCount < wanted.size()) {
			LoopSourceLocation location;
			auto result = locateLoopSource(frame, location);
			if (result == LoopReadResult::PENDING)
				break;
			if (result == LoopReadResult::INVALID)
				return Error::FILE_CORRUPTED;
			uint32_t width = location.sample->byteDepth * location.sample->numChannels;
			if (!width || !location.count)
				return Error::FILE_CORRUPTED;
			uint64_t address = location.sample->audioDataStartPosBytes + uint64_t(location.frame) * width;
			for (uint32_t index = address / Cluster::size; index <= (address + width - 1) / Cluster::size; ++index) {
				bool found = std::any_of(wanted.begin(), wanted.begin() + wantedCount,
				                         [&](auto key) { return key.sample == location.sample && key.index == index; });
				if (!found && wantedCount < wanted.size())
					wanted[wantedCount++] = {location.sample, index};
			}
			uint32_t count = std::min(
			    {remaining, location.count, std::max<uint32_t>(1, (Cluster::size - address % Cluster::size) / width)});
			remaining -= count;
			frame = (frame + count) % loopFrames_;
		}
		return Error::NONE;
	};
	if (loopSource_) {
		// An audible read can outrun the writer while the untouched tail copies.
		// Pin it first, including both clusters when a PCM frame straddles one.
		auto error = addWindow(loopPlaybackCursor_, 1);
		if (error != Error::NONE)
			return error;
		if (!loopOverdubReady()) {
			error = addWindow(loopFrameCursor_, loopFrames_);
			if (error != Error::NONE)
				return error;
		}
	}
	else {
		auto error = addWindow(loopFrameCursor_, loopFrames_);
		if (error != Error::NONE)
			return error;
	}
	for (auto& entry : loopSourceCache_) {
		bool needed = entry.cluster && std::any_of(wanted.begin(), wanted.begin() + wantedCount, [&](auto key) {
			              return key.sample == entry.sample && key.index == entry.index;
		              });
		if (entry.cluster && !needed) {
			audioFileManager.removeReasonFromCluster(*entry.cluster, "LSSX");
			entry = {};
		}
	}
	for (size_t i = 0; i < wantedCount; ++i) {
		uint32_t index = wanted[i].index;
		auto* source = wanted[i].sample;
		if (std::any_of(loopSourceCache_.begin(), loopSourceCache_.end(), [=](auto const& entry) {
			    return entry.cluster && entry.index == index && entry.sample == source;
		    }))
			continue;
		auto target = std::find_if(loopSourceCache_.begin(), loopSourceCache_.end(),
		                           [](auto const& entry) { return entry.cluster == nullptr; });
		if (target == loopSourceCache_.end() || index >= static_cast<uint32_t>(source->clusters.getNumElements()))
			return Error::SD_CARD;
		Cluster* cluster = source->clusters.getElement(index)->getCluster(source, index, CLUSTER_ENQUEUE);
		if (!cluster)
			return Error::INSUFFICIENT_RAM;
		*target = {cluster, index, source};
	}
	return Error::NONE;
}

SampleRecorder::LoopReadResult SampleRecorder::readLoopSourceFrame(uint32_t frame, StereoSample& destination) {
	LoopSourceLocation location;
	auto result = locateLoopSource(frame, location);
	if (result != LoopReadResult::READY)
		return result;
	auto* source = location.sample;
	if (!source->byteDepth || source->byteDepth > 4 || !source->numChannels || source->numChannels > 2)
		return LoopReadResult::INVALID;
	uint64_t address =
	    source->audioDataStartPosBytes + uint64_t(location.frame) * source->byteDepth * source->numChannels;
	destination = {};
	for (uint32_t channel = 0; channel < source->numChannels; ++channel) {
		uint32_t value = 0;
		for (uint32_t byte = 0; byte < source->byteDepth; ++byte, ++address) {
			uint32_t index = address / Cluster::size;
			auto entry = std::find_if(loopSourceCache_.begin(), loopSourceCache_.end(), [=](auto const& candidate) {
				return candidate.cluster && candidate.index == index && candidate.sample == source;
			});
			// Audio callbacks do not yield: a resident descriptor can be borrowed
			// for this read even when it lies beyond our prefetch window. The
			// retained generation chain keeps its Sample alive throughout.
			Cluster* cluster = entry != loopSourceCache_.end() ? entry->cluster
			                   : index < static_cast<uint32_t>(source->clusters.getNumElements())
			                       ? source->clusters.getElement(index)->cluster
			                       : nullptr;
			if (!cluster || !cluster->loaded)
				return LoopReadResult::PENDING;
			value |= static_cast<uint32_t>(static_cast<uint8_t>(cluster->data[address % Cluster::size]))
			         << (8 * (4 - source->byteDepth + byte));
		}
		int32_t sampleValue = std::bit_cast<int32_t>(value);
		if (channel == 0)
			destination.l = destination.r = sampleValue;
		else if (channel == 1)
			destination.r = sampleValue;
	}
	return LoopReadResult::READY;
}

SampleRecorder::LoopReadResult SampleRecorder::readLoopFallbackFrame(uint32_t frame, StereoSample& destination) {
	SampleRecorder* owner = this;
	for (uint32_t depth = 0; depth < 18; ++depth) {
		if (!owner->loopSource_ || !owner->loopFrames_)
			return LoopReadResult::INVALID;
		frame = (owner->loopSourceFrame_ + frame) % owner->loopFrames_;
		auto* parent = owner->loopSourceRecorder_;
		uint32_t appliedPrefix = parent ? parent->numSamplesCaptured : 0;
		if (!parent || frame < appliedPrefix) {
			Sample* source = owner->loopSource_;
			if (!source->byteDepth || source->byteDepth > 4 || !source->numChannels || source->numChannels > 2)
				return LoopReadResult::INVALID;
			uint64_t address = source->audioDataStartPosBytes
			                   + uint64_t(owner->loopSourceStart_ + frame) * source->byteDepth * source->numChannels;
			destination = {};
			for (uint32_t channel = 0; channel < source->numChannels; ++channel) {
				uint32_t value = 0;
				for (uint32_t byte = 0; byte < source->byteDepth; ++byte, ++address) {
					uint32_t index = address / Cluster::size;
					if (index >= static_cast<uint32_t>(source->clusters.getNumElements()))
						return LoopReadResult::INVALID;
					auto* cluster = source->clusters.getElement(index)->cluster;
					if (!cluster || !cluster->loaded)
						return LoopReadResult::PENDING;
					value |= static_cast<uint32_t>(static_cast<uint8_t>(cluster->data[address % Cluster::size]))
					         << (8 * (4 - source->byteDepth + byte));
				}
				int32_t sampleValue = std::bit_cast<int32_t>(value);
				if (channel == 0)
					destination.l = destination.r = sampleValue;
				else
					destination.r = sampleValue;
			}
			return LoopReadResult::READY;
		}
		if (!parent->isLoopOverdub())
			return LoopReadResult::PENDING;
		owner = parent;
	}
	return LoopReadResult::INVALID;
}

void SampleRecorder::renderLoopFallbackBacking(std::span<StereoSample> backing, size_t firstFrame) {
	loopFallbackBackingRendered_ = true;
	for (size_t i = firstFrame; i < backing.size(); ++i) {
		StereoSample source{};
		if (readLoopFallbackFrame(loopPlaybackCursor_, source) == LoopReadResult::READY) {
			backing[i] = {source.l >> deluge::loop_station::kCaptureGainShift,
			              source.r >> deluge::loop_station::kCaptureGainShift};
		}
		else {
			backing[i] = {};
		}
		loopPlaybackCursor_ = (loopPlaybackCursor_ + 1) % loopFrames_;
		if (loopPlaybackFramesRendered_ != UINT32_MAX)
			++loopPlaybackFramesRendered_;
	}
}

bool SampleRecorder::loopFrameReady() {
	uint64_t offset = sample->audioDataStartPosBytes + uint64_t(loopFrameCursor_) * 6;
	for (uint32_t byte = 0; byte < 6; ++byte) {
		auto* entry = loopCacheAt((offset + byte) / Cluster::size);
		if (!entry || !entry->cluster->loaded)
			return false;
	}
	if (numSamplesCaptured < loopFrames_) {
		StereoSample previous{};
		auto error = readLoopSourceFrame(loopFrameCursor_, previous);
		if (error == LoopReadResult::PENDING)
			return false; // CLUSTER_ENQUEUE is not a synchronous read.
		if (error != LoopReadResult::READY) {
			captureOutputBackingMissing = true;
			return false;
		}
	}
	return true;
}

void SampleRecorder::drainLoopInput(uint32_t budget) {
	if (loopDrainingInput_ || hadCardError || terminalError != Error::NONE || status >= RecorderStatus::COMPLETE)
		return;
	loopDrainingInput_ = true;
	while (budget-- && loopPendingCount_ && !captureOutputBackingMissing && loopFrameReady()) {
		auto& frame = loopPendingFrames_[loopPendingHead_];
		processLoopOverdub({&frame.backing, 1}, {&frame.input, 1});
		loopPendingHead_ = (loopPendingHead_ + 1) % loopPendingCapacity_;
		--loopPendingCount_;
	}
	loopDrainingInput_ = false;
}

void SampleRecorder::processLoopOverdub(std::span<StereoSample> backing, std::span<const StereoSample> input,
                                        bool backgroundCopy, bool audibleDeadline) {
	using deluge::loop_station::kCaptureGainShift;
	using deluge::loop_station::restoreCaptureGain;
	using deluge::loop_station::saturatingAdd;
	loopFallbackBackingRendered_ = false;
	if (captureOutputBackingMissing || hadCardError || terminalError != Error::NONE || status == RecorderStatus::ABORTED
	    || status == RecorderStatus::AWAITING_DELETION) {
		if (!backgroundCopy && loopSource_ && loopFrames_)
			renderLoopFallbackBacking(backing, 0);
		return;
	}
	if (loopPreparing_ || !loopFrames_)
		return;
	if (loopSource_ && !backgroundCopy && !loopDrainingInput_) {
		drainLoopInput();
		// Background copying owns the writer cursor after the input gate closes.
		// Audible playback keeps its own cursor, so new additions do not vanish
		// while the untouched tail or a preceding revision is still completing.
		for (size_t i = 0; i < backing.size(); ++i) {
			auto& frame = backing[i];
			if (loopPlaybackCursor_ < numSamplesCaptured) {
				uint64_t offset = sample->audioDataStartPosBytes + static_cast<uint64_t>(loopPlaybackCursor_) * 6;
				uint32_t values[2]{};
				for (uint32_t byte = 0; byte < 6; ++byte) {
					auto address = offset + byte;
					auto* entry = loopCacheAt(address / Cluster::size);
					if (!entry || !entry->cluster->loaded) {
						// Older backing does not contain these recorded additions.
						// Never silently substitute it for the current working audio.
						traceLifecycle(LifecycleEvent::CACHE_UNDERRUN, nullptr, address / Cluster::size);
						captureOutputBackingMissing = true;
						renderLoopFallbackBacking(backing, i);
						return;
					}
					values[byte / 3] |=
					    static_cast<uint32_t>(static_cast<uint8_t>(entry->cluster->data[address % Cluster::size]))
					    << (8 * (byte % 3 + 1));
				}
				frame = {std::bit_cast<int32_t>(values[0]) >> kCaptureGainShift,
				         std::bit_cast<int32_t>(values[1]) >> kCaptureGainShift};
			}
			else {
				StereoSample previous{};
				auto error = readLoopSourceFrame(loopPlaybackCursor_, previous);
				if (error == LoopReadResult::READY)
					frame = {previous.l >> kCaptureGainShift, previous.r >> kCaptureGainShift};
				else if (error != LoopReadResult::PENDING || audibleDeadline) {
					captureOutputBackingMissing = true;
					renderLoopFallbackBacking(backing, i);
					return;
				}
			}
			if (!loopEnding_ && !captureOutputBackingMissing) {
				if (loopPendingCount_ == loopPendingCapacity_) {
					terminalError = Error::INSUFFICIENT_RAM;
					return;
				}
				auto tail = (loopPendingHead_ + loopPendingCount_) % loopPendingCapacity_;
				loopPendingFrames_[tail] = {frame, captureOutputInputActive ? input[i] : StereoSample{}};
				if (!loopAcceptedOriginSet_) {
					loopAcceptedOriginFrame_ = loopPlaybackCursor_;
					loopAcceptedOriginSet_ = true;
				}
				++loopAcceptedFrames_;
				++loopPendingCount_;
				drainLoopInput(1);
			}
			loopPlaybackCursor_ = (loopPlaybackCursor_ + 1) % loopFrames_;
			if (loopPlaybackFramesRendered_ != UINT32_MAX)
				++loopPlaybackFramesRendered_;
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
				if (!backgroundCopy && !loopDrainingInput_ && loopSource_)
					renderLoopFallbackBacking(backing, frame);
				return;
			}
			bytes[byte] = entries[byte]->cluster->data + address % Cluster::size;
		}
		StereoSample previous{restoreCaptureGain(backing[frame].l), restoreCaptureGain(backing[frame].r)};
		if (loopSource_ && numSamplesCaptured < loopFrames_) {
			StereoSample sourcePrevious{};
			auto error = readLoopSourceFrame(loopFrameCursor_, sourcePrevious);
			if (error != LoopReadResult::READY) {
				captureOutputBackingMissing = true;
				return;
			}
			previous = sourcePrevious;
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
			StereoSample added =
			    !backgroundCopy && (captureOutputInputActive || loopDrainingInput_) ? input[frame] : StereoSample{};
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
		if (loopDrainingInput_)
			++loopAppliedFrames_;
		if (!backgroundCopy && !loopDrainingInput_)
			loopPlaybackCursor_ = loopFrameCursor_;
	}
}

Error SampleRecorder::copyLoopRemainder() {
	// Source resolution includes immutable prefixes of older, still-saving
	// generations. Never wait for an entire parent WAV to be materialized.
	for (uint32_t i = 0; i < 128 && numSamplesCaptured < loopFrames_; ++i) {
		if (!loopFrameReady())
			return Error::NONE;
		StereoSample frame{};
		processLoopOverdub({&frame, 1}, {}, true);
	}
	return terminalError;
}

void SampleRecorder::sealLoopOverdub() {
	stopLoopOverdubInput();
	recorderBudgetLease_.finishCapture();
	loopSealedPrefix_ = std::min(loopFrames_, numSamplesCaptured + loopPendingCount_);
	loopSealed_ = true;
	status = RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING;
	if (outputRecordingFrom)
		outputRecordingFrom->removeRecorder(this);
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
	drainLoopInput();
	if (loopEnding_ && loopSource_ && !loopPendingCount_ && !loopOverdubReady()) {
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
	if (status == RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING && loopOverdubReady()) {
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
	if (loopPendingFrames_) {
		delugeDealloc(loopPendingFrames_);
		loopPendingFrames_ = nullptr;
	}
	loopPendingCount_ = loopPendingCapacity_ = 0;
	for (auto& entry : loopSourceCache_) {
		if (entry.cluster) {
			audioFileManager.removeReasonFromCluster(*entry.cluster, "LSSR");
			entry = {};
		}
	}
	if (loopSourceRecorder_) {
		--loopSourceRecorder_->loopDependentReaders_;
		loopSourceRecorder_->releaseLoopReader();
		loopSourceRecorder_ = nullptr;
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
