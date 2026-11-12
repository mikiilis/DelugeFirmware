#include "archive.h"
#include <algorithm>
#include <cstring>
#include <limits>
namespace loop_station {
namespace {
void le16(std::byte* p, uint16_t v) {
	for (unsigned i = 0; i < 2; ++i)
		p[i] = std::byte(v >> (8 * i));
}
void le32(std::byte* p, uint32_t v) {
	for (unsigned i = 0; i < 4; ++i)
		p[i] = std::byte(v >> (8 * i));
}
void wavHeader(std::byte* p, FrameIndex frames) {
	std::memcpy(p, "RIFF", 4);
	le32(p + 4, 36 + frames * 6);
	std::memcpy(p + 8, "WAVEfmt ", 8);
	le32(p + 16, 16);
	le16(p + 20, 1);
	le16(p + 22, 2);
	le32(p + 24, kSampleRate);
	le32(p + 28, kSampleRate * 6);
	le16(p + 32, 6);
	le16(p + 34, 24);
	std::memcpy(p + 36, "data", 4);
	le32(p + 40, frames * 6);
}
} // namespace
Status Archive::request(SlotId slot, Generation generation) {
	if (!validSlot(slot) || !generation)
		return Status::Invalid;
	if (!storage_ || closed_)
		return Status::Unavailable;
	if (sequence_ == std::numeric_limits<Generation>::max())
		return Status::Unavailable;
	auto& job = jobs_[slot];
	if (job.snapshot.valid()) {
		snapshots_.release(job.snapshot);
		job.snapshot = {};
	}
	if (job.state == ArchiveState::ReadyForPublication)
		job.discard = true;
	job.request = ++sequence_;
	job.pendingGeneration = generation;
	job.pending = true;
	job.state = ArchiveState::Pending;
	job.result = Status::Ok;
	job.diagnostics = {};
	if (!rollback_.provisional(slot)) {
		SnapshotId snapshot;
		const auto status = snapshots_.request(slot, generation, snapshot);
		if (status != Status::Ok) {
			job.pending = false;
			job.state = ArchiveState::Failed;
			job.result = status;
			return status;
		}
		job.snapshot = snapshot;
	}
	return Status::Ok;
}
void Archive::cancel(SlotId slot) {
	if (!validSlot(slot))
		return;
	auto& job = jobs_[slot];
	if (job.snapshot.valid()) {
		snapshots_.release(job.snapshot);
		job.snapshot = {};
	}
	if (job.state == ArchiveState::ReadyForPublication)
		job.discard = true;
	job.pending = false;
	job.request = 0;
	job.state = ArchiveState::Cancelled;
	job.result = Status::Stale;
}
void Archive::cancelAll() {
	for (SlotId s = 0; s < kTrackCount; ++s)
		cancel(s);
}
void Archive::close() {
	closed_ = true;
	cancelAll();
	// Inactive candidates may become bounded orphans for manifest recovery.
	// Active streams retain this owner until their resumable abort finishes.
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		auto& job = jobs_[slot];
		if (slot == active_ || !job.discard || job.discardStarted)
			continue;
		if (storage_)
			storage_->transfer(slot, job.candidate.identity);
		job.candidate = {};
		job.discard = false;
	}
}
ArchiveState Archive::state(SlotId slot) const {
	return validSlot(slot) ? jobs_[slot].state : ArchiveState::Failed;
}
Status Archive::result(SlotId slot) const {
	return validSlot(slot) ? jobs_[slot].result : Status::Invalid;
}
unsigned Archive::queued() const {
	unsigned count = 0;
	for (const auto& j : jobs_)
		count += j.pending;
	return count;
}
bool Archive::idle() const {
	if (active_ != kInvalidSlot || servicing_)
		return false;
	for (const auto& j : jobs_)
		if (j.pending || j.discard)
			return false;
	return true;
}
bool Archive::take(SlotId slot, ArchiveCandidate& candidate) {
	if (!validSlot(slot))
		return false;
	auto& job = jobs_[slot];
	if (job.state != ArchiveState::ReadyForPublication || job.discard || job.pending)
		return false;
	if (job.request != job.candidate.identity
	    || pool_.length({slot, job.candidate.phraseGeneration}) != job.candidate.frames) {
		cancel(slot);
		return false;
	}
	candidate = job.candidate;
	job.candidate = {};
	job.state = ArchiveState::Idle;
	storage_->transfer(slot, candidate.identity);
	return true;
}
bool Archive::current() const {
	if (!validSlot(active_))
		return false;
	const auto& job = jobs_[active_];
	return job.request == activeRequest_ && !job.pending && job.state == ArchiveState::Writing
	       && snapshots_.valid(job.snapshot)
	       && pool_.length({active_, job.candidate.phraseGeneration}) == job.candidate.frames;
}
void Archive::finish(Status status) {
	auto& job = jobs_[active_];
	if (status != Status::Ok || !current()) {
		retirementStatus_ = status == Status::Ok ? Status::Stale : status;
		aborting_ = true;
		storage_->abort(); // Request only; storage cleanup starts on a later service.
		snapshots_.release(job.snapshot);
		job.snapshot = {};
		return;
	}
	if (const char* path = storage_->candidatePath(active_, activeRequest_))
		std::strncpy(job.candidate.path.data(), path, job.candidate.path.size() - 1);
	snapshots_.release(job.snapshot);
	job.snapshot = {};
	job.result = Status::Ok;
	job.diagnostics = storage_->diagnostics();
	job.state = ArchiveState::ReadyForPublication;
	active_ = kInvalidSlot;
	activeRequest_ = 0;
	beginComplete_ = headerWritten_ = false;
	stagedBytes_ = stagedWritten_ = stagedFrames_ = 0;
}
void Archive::retire() {
	auto& job = jobs_[active_];
	job.cleanupDiagnostics = storage_->cleanupDiagnostics(active_);
	if (job.request == activeRequest_ || job.request == 0) {
		job.diagnostics = storage_->diagnostics();
		job.result = job.request ? retirementStatus_ : Status::Stale;
		job.state = job.request ? ArchiveState::Failed : ArchiveState::Cancelled;
	}
	job.candidate = {};
	active_ = kInvalidSlot;
	activeRequest_ = 0;
	aborting_ = false;
	beginComplete_ = headerWritten_ = false;
	stagedBytes_ = stagedWritten_ = stagedFrames_ = 0;
}
void Archive::service(uint32_t maxBytes) {
	if (servicing_ || !storage_)
		return;
	servicing_ = true;
	struct Guard {
		bool& flag;
		~Guard() { flag = false; }
	} guard{servicing_};
	if (aborting_) {
		const auto status = storage_->serviceAbort();
		if (status != Status::Busy)
			retire();
		return;
	}
	// Candidate deletion is foreground work too; clear ownership before yielding.
	for (SlotId s = 0; s < kTrackCount; ++s) {
		auto& job = jobs_[s];
		if (!job.discard)
			continue;
		if (!job.discardStarted) {
			storage_->discard(s, job.candidate.identity);
			job.discardStarted = true;
		}
		const auto status = storage_->serviceDiscard(s, job.candidate.identity);
		job.cleanupDiagnostics = storage_->cleanupDiagnostics(s);
		if (status != Status::Busy) {
			job.candidate = {};
			job.discard = job.discardStarted = false;
		}
		return;
	}
	if (validSlot(active_) && !current()) {
		finish(Status::Stale);
		return;
	}
	if (!validSlot(active_)) {
		if (maxBytes < 44)
			return;
		for (unsigned i = 0; i < kTrackCount; ++i) {
			const auto slot = next_;
			next_ = (next_ + 1) % kTrackCount;
			auto& job = jobs_[slot];
			if (!job.pending || rollback_.provisional(slot))
				continue;
			SnapshotId snapshot = job.snapshot;
			const auto status =
			    snapshot.valid() ? Status::Ok : snapshots_.request(slot, job.pendingGeneration, snapshot);
			job.pending = false;
			if (status != Status::Ok) {
				job.state = ArchiveState::Failed;
				job.result = status;
				continue;
			}
			job.snapshot = snapshot;
			job.candidate = {slot, job.request, job.pendingGeneration, pool_.length({slot, job.pendingGeneration})};
			if (job.candidate.frames > (UINT32_MAX - 36) / 6) {
				snapshots_.release(snapshot);
				job.snapshot = {};
				job.state = ArchiveState::Failed;
				job.result = Status::Invalid;
				continue;
			}
			job.state = ArchiveState::Writing;
			active_ = slot;
			activeRequest_ = job.request;
			offset_ = 0;
			break;
		}
	}
	if (!validSlot(active_))
		return;
	if (!beginComplete_) {
		if (maxBytes < 44)
			return;
		const auto status = storage_->begin(active_, activeRequest_);
		if (!current()) {
			finish(Status::Stale);
			return;
		}
		if (status == Status::Busy)
			return;
		if (status != Status::Ok) {
			finish(status);
			return;
		}
		beginComplete_ = true;
		return;
	}
	const auto length = jobs_[active_].candidate.frames;
	if (!stagedBytes_ && !headerWritten_) {
		if (maxBytes < 44)
			return;
		wavHeader(bytes_.data(), length);
		stagedBytes_ = 44;
		stagedWritten_ = 0;
		stagedFrames_ = 0;
	}
	if (!stagedBytes_ && offset_ < length) {
		const auto count = std::min<size_t>({frames_.size(), maxBytes / 6, length - offset_});
		if (!count)
			return;
		const auto status = snapshots_.read(jobs_[active_].snapshot, offset_, std::span(frames_).first(count));
		if (status != Status::Ok || !current()) {
			finish(Status::Stale);
			return;
		}
		for (size_t i = 0; i < count; ++i) {
			const auto l = uint32_t(frames_[i].l) >> 8, r = uint32_t(frames_[i].r) >> 8;
			for (unsigned n = 0; n < 3; ++n) {
				bytes_[i * 6 + n] = std::byte(l >> (8 * n));
				bytes_[i * 6 + n + 3] = std::byte(r >> (8 * n));
			}
		}
		stagedBytes_ = count * 6;
		stagedWritten_ = 0;
		stagedFrames_ = count;
	}
	if (stagedBytes_) {
		const auto count = std::min<uint32_t>(stagedBytes_ - stagedWritten_, maxBytes);
		if (!count)
			return;
		size_t written = 0;
		const auto status = storage_->write(std::span(bytes_).subspan(stagedWritten_, count), written);
		if (!current()) {
			finish(Status::Stale);
			return;
		}
		if (written > count || (status != Status::Ok && status != Status::Busy)
		    || (status == Status::Ok && written != count)) {
			finish(status == Status::Ok || status == Status::Busy ? Status::StorageError : status);
			return;
		}
		stagedWritten_ += written;
		if (stagedWritten_ == stagedBytes_) {
			if (!headerWritten_)
				headerWritten_ = true;
			else
				offset_ += stagedFrames_;
			stagedBytes_ = stagedWritten_ = stagedFrames_ = 0;
		}
		return;
	}
	if (offset_ == length) {
		if (!current()) {
			finish(Status::Stale);
			return;
		}
		const auto status = storage_->finalize();
		if (!current())
			finish(Status::Stale);
		else if (status != Status::Busy)
			finish(status);
	}
}
} // namespace loop_station
