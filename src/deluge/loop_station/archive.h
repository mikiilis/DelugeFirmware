#pragma once
#include "snapshot.h"

namespace loop_station {
enum class ArchiveState : uint8_t { Idle, Pending, Writing, ReadyForPublication, Failed, Cancelled };
enum class MediaError : uint8_t { None, Absent, Full, Protected, Io };
struct StorageDiagnostics {
	MediaError error = MediaError::None;
	uint32_t nativeError = 0;
	uint64_t bytesWritten = 0;
};
struct ArchiveCandidate {
	SlotId slot = kInvalidSlot;
	Generation identity = 0;
	Generation phraseGeneration = 0;
	FrameIndex frames = 0;
	std::array<char, 80> path{};
};
class ArchiveStorage {
public:
	virtual ~ArchiveStorage() = default;
	// Busy means setup/collision lookup yielded; retry the same identity next service.
	virtual Status begin(SlotId slot, Generation identity) = 0;
	virtual Status write(std::span<const std::byte>, size_t& written) = 0;
	// Busy resumes finalization at its next storage operation on a later service.
	virtual Status finalize() = 0;
	// Non-yielding abort request; serviceAbort performs at most one storage operation.
	virtual void abort() = 0;
	virtual Status serviceAbort() { return Status::Ok; }
	// Non-yielding discard request; serviceDiscard may return Busy between bounded steps.
	virtual void discard(SlotId, Generation) = 0;
	virtual Status serviceDiscard(SlotId, Generation) { return Status::Ok; }
	// Non-yielding ownership transfer; subsequent abort/discard cannot remove this file.
	virtual void transfer(SlotId, Generation) {}
	virtual const char* candidatePath(SlotId, Generation) const { return nullptr; }
	// Foreground-only bounded read. A valid prefix is reported explicitly; the
	// total declared frame count is returned after the WAV header is validated.
	virtual Status readCheckpoint(SlotId, Generation, FrameIndex, std::span<Frame>, size_t& framesRead,
	                              FrameIndex& totalFrames) {
		framesRead = 0;
		totalFrames = 0;
		return Status::SourceUnavailable;
	}
	virtual void cancelCheckpointRead(SlotId, Generation) {}
	virtual Status serviceCheckpointReadCleanup() { return Status::Ok; }
	virtual bool checkpointReadIdle() const { return true; }
	virtual MediaError error() const { return MediaError::Io; }
	virtual StorageDiagnostics diagnostics() const { return {error(), 0, 0}; }
	virtual StorageDiagnostics cleanupDiagnostics(SlotId) const { return {}; }
};
// Serialized foreground control. Storage calls may yield to audio/control, so no
// live arena pointer crosses them. take() transfers an unreferenced candidate to
// the manifest owner; ReadyForPublication alone never means a saved song.
class Archive {
public:
	Archive(PagePool& pool, GestureRollback& rollback, SnapshotStore& snapshots)
	    : pool_(pool), rollback_(rollback), snapshots_(snapshots) {}
	// Configure once before the first request; a job cannot switch backends while yielding.
	Status setStorage(ArchiveStorage* storage) {
		if (storage_ == storage)
			return Status::Ok;
		if (sequence_ || servicing_)
			return Status::Busy;
		storage_ = storage;
		return Status::Ok;
	}
	Status request(SlotId, Generation);
	void service(uint32_t maxBytes);
	void cancel(SlotId);
	void cancelAll();
	void close();
	ArchiveState state(SlotId slot) const;
	Status result(SlotId slot) const;
	StorageDiagnostics diagnostics(SlotId slot) const {
		return validSlot(slot) ? jobs_[slot].diagnostics : StorageDiagnostics{};
	}
	StorageDiagnostics cleanupDiagnostics(SlotId slot) const {
		return validSlot(slot) ? jobs_[slot].cleanupDiagnostics : StorageDiagnostics{};
	}
	Status readCheckpoint(SlotId slot, Generation identity, FrameIndex first, std::span<Frame> frames,
	                      size_t& framesRead, FrameIndex& totalFrames) {
		framesRead = 0;
		totalFrames = 0;
		return storage_ ? storage_->readCheckpoint(slot, identity, first, frames, framesRead, totalFrames)
		                : Status::SourceUnavailable;
	}
	void cancelCheckpointRead(SlotId slot, Generation identity) {
		if (storage_)
			storage_->cancelCheckpointRead(slot, identity);
	}
	Status serviceCheckpointReadCleanup() { return storage_ ? storage_->serviceCheckpointReadCleanup() : Status::Ok; }
	bool checkpointReadIdle() const { return !storage_ || storage_->checkpointReadIdle(); }
	bool take(SlotId, ArchiveCandidate&);
	// READY candidates are immutable unreferenced files. Teardown may leave them
	// for manifest recovery cleanup; destruction never performs SD I/O.
	bool idle() const;
	unsigned queued() const;

private:
	struct Job {
		ArchiveCandidate candidate{};
		SnapshotId snapshot{};
		ArchiveState state = ArchiveState::Idle;
		Status result = Status::Ok;
		StorageDiagnostics diagnostics{}, cleanupDiagnostics{};
		Generation request = 0;
		Generation pendingGeneration = 0;
		bool pending = false, discard = false, discardStarted = false;
	};
	bool current() const;
	void finish(Status);
	void retire();
	PagePool& pool_;
	GestureRollback& rollback_;
	SnapshotStore& snapshots_;
	ArchiveStorage* storage_ = nullptr;
	std::array<Job, kTrackCount> jobs_{};
	std::array<Frame, SnapshotStore::kStagingFrames> frames_{};
	std::array<std::byte, SnapshotStore::kStagingFrames * 6> bytes_{};
	Generation sequence_ = 0, activeRequest_ = 0;
	SlotId active_ = kInvalidSlot, next_ = 0;
	FrameIndex offset_ = 0, stagedFrames_ = 0;
	uint32_t stagedBytes_ = 0, stagedWritten_ = 0;
	Status retirementStatus_ = Status::Ok;
	bool closed_ = false, aborting_ = false;
	bool servicing_ = false, beginComplete_ = false, headerWritten_ = false;
};
} // namespace loop_station
