#include "history.h"
#include <algorithm>
#include <limits>
namespace loop_station {
Status History::initialize(std::span<std::byte> storage) {
	return pages_.initialize(storage);
}
unsigned History::retainedSessions() const {
	unsigned count = 0;
	for (const auto& session : sessions_)
		count += session.identity != 0;
	return count;
}
void History::losePages(unsigned id) {
	auto& session = sessions_[id];
	session.cleanupQueued = true;
	session.pages = 0;
	session.complete = false;
	session.lastPage = UINT32_MAX;
}
void History::discard(unsigned id) {
	if (id == none || id >= kMaxSessions || !sessions_[id].identity || sessions_[id].retiring)
		return;
	auto& session = sessions_[id];
	for (auto& slot : slots_) {
		if (slot.active == id)
			slot.active = none;
		if (slot.head == id)
			slot.head = session.next;
		if (slot.tail == id)
			slot.tail = session.previous;
		if (slot.cursor == id)
			slot.cursor = session.previous;
		if (slot.latest == id)
			slot.latest = session.previous;
	}
	if (session.previous != none)
		sessions_[session.previous].next = session.next;
	if (session.next != none)
		sessions_[session.next].previous = session.previous;
	session.previous = session.next = none;
	enqueueRetirement(id);
}

void History::enqueueRetirement(unsigned root) {
	if (root == none || root >= kMaxSessions || !sessions_[root].identity)
		return;
	for (auto& retirement : retirements_)
		if (retirement.active && retirement.current == root && retirement.identity == sessions_[root].identity) {
			return;
		}
	for (auto& retirement : retirements_)
		if (!retirement.active) {
			retirement = {root, sessions_[root].identity, true};
			return;
		}
}
void History::beginSession(SlotId slot, Generation generation) {
	if (!pages_.ready() || !validSlot(slot))
		return;
	const PhraseHandle handle{slot, generation};
	if (!pool_.length(handle) || rollback_.boundGeneration(slot) != generation)
		return;
	if (slots_[slot].active != none)
		return; // Repeated gate/checkpoint requests coalesce.
	cancelRestore(slot);
	invalidateRedo(slot);
	if (sequence_ == std::numeric_limits<Generation>::max()) {
		// Exhaustion degrades to unavailable history rather than recycling identities.
		discard(slots_[slot].latest);
		return;
	}
	unsigned free = none;
	for (unsigned i = 0; i < kMaxSessions; ++i) {
		if (!sessions_[i].identity) {
			free = i;
			break;
		}
	}
	if (free == none) {
		++pressure_;
		// This edit cannot be tracked until a descriptor is retired. Fence this
		// slot first, then reclaim one oldest reachable boundary from another slot
		// so later edits can recover without crossing the untracked edit.
		resetSlot(slot);
		SlotId oldestSlot = kInvalidSlot;
		Generation oldestIdentity = std::numeric_limits<Generation>::max();
		for (SlotId owner = 0; owner < kTrackCount; ++owner) {
			const auto root = slots_[owner].head;
			if (root == none || root >= kMaxSessions)
				continue;
			const auto& candidate = sessions_[root];
			if (!candidate.identity || candidate.active || candidate.retiring)
				continue;
			if (candidate.identity < oldestIdentity) {
				oldestIdentity = candidate.identity;
				oldestSlot = owner;
			}
		}
		if (validSlot(oldestSlot)) {
			const auto root = slots_[oldestSlot].head;
			if (sessions_[root].complete)
				discard(root);
			else
				resetSlot(oldestSlot); // Never remove an incomplete fence alone.
		}
		return;
	}
	auto& session = sessions_[free];
	session = {};
	session.handle = handle;
	session.identity = ++sequence_;
	session.revision = rollback_.readRevision(slot);
	session.active = true;
	slots_[slot].active = free;
}
void History::beforeWrite(SlotId slot, FrameIndex index, Frame original) {
	if (!validSlot(slot) || slots_[slot].active == none)
		return;
	const auto id = slots_[slot].active;
	auto& session = sessions_[id];
	session.changed = true;
	if (!session.complete)
		return;
	if (!pool_.length(session.handle) || session.revision != rollback_.readRevision(slot)) {
		losePages(id);
		return;
	}
	const auto page = index / (kPageBytes / sizeof(Frame));
	if (session.lastPage == page)
		return;
	auto entry = pages_.find(session.identity, page);
	if (entry == detail::BeforeImages::none) {
		if (pages_.count() == pages_.capacity()) {
			++pressure_;
			unsigned oldest = none;
			for (SlotId owner = 0; owner < kTrackCount; ++owner) {
				const auto candidate = slots_[owner].head;
				if (candidate != none && candidate != id && sessions_[candidate].identity
				    && !sessions_[candidate].active && !sessions_[candidate].retiring && sessions_[candidate].complete
				    && sessions_[candidate].changed
				    && (oldest == none || sessions_[candidate].identity < sessions_[oldest].identity))
					oldest = candidate;
			}
			if (oldest != none)
				discard(oldest); // Queue the oldest volatile boundary for foreground reclamation.
			losePages(id);
			return;
		}
		entry = pages_.claim(session.identity, page);
		if (entry == detail::BeforeImages::none
		    || !pages_.capture(entry, pool_, rollback_, session.handle, index, original)) {
			losePages(id);
			return;
		}
		++session.pages;
	}
	session.lastPage = page;
}
void History::endSession(SlotId slot) {
	if (!validSlot(slot) || slots_[slot].active == none)
		return;
	const auto id = slots_[slot].active;
	auto& session = sessions_[id];
	session.active = false;
	slots_[slot].active = none;
	if (!session.changed) {
		discard(id);
		return;
	}
	if (!session.complete) {
		const auto oldHead = slots_[slot].head;
		if (oldHead != none)
			enqueueRetirement(oldHead);
		session.previous = none;
		slots_[slot].head = id;
	}
	else
		session.previous = slots_[slot].tail;
	session.next = none;
	if (session.previous != none)
		sessions_[session.previous].next = id;
	else if (slots_[slot].head == none)
		slots_[slot].head = id;
	slots_[slot].tail = id;
	slots_[slot].cursor = id;
	slots_[slot].latest = id;
	slots_[slot].stateRevision = rollback_.readRevision(slot);
}
bool History::canUndo(SlotId slot) const {
	if (!validSlot(slot) || slots_[slot].active != none || slots_[slot].cursor == none
	    || slots_[slot].stateRevision != rollback_.readRevision(slot))
		return false;
	const auto& session = sessions_[slots_[slot].cursor];
	// Never search backwards past a missing/incomplete latest step.
	const auto length = pool_.length(pool_.sealedHandle(slot));
	return session.identity && session.complete && session.changed && length && pool_.canBeginDetached(slot, length);
}

bool History::canRedo(SlotId slot) const {
	if (!validSlot(slot) || slots_[slot].active != none || slots_[slot].stateRevision != rollback_.readRevision(slot))
		return false;
	const auto next = slots_[slot].cursor == none ? slots_[slot].head : sessions_[slots_[slot].cursor].next;
	return next != none && sessions_[next].identity && sessions_[next].complete && sessions_[next].changed
	       && pool_.length(sessions_[next].afterVersion) != 0;
}

Status History::requestRestore(SlotId slot, bool redo, RestoreId& id) {
	id = {};
	if (!validSlot(slot) || !pages_.ready())
		return Status::Invalid;
	if (restore_.active)
		return Status::Busy;
	if (slots_[slot].active != none || rollback_.provisional(slot) || rollback_.pending(slot))
		return Status::Busy;
	if (redo) {
		if (!canRedo(slot))
			return Status::SourceUnavailable;
	}
	else {
		if (slots_[slot].active != none || slots_[slot].cursor == none
		    || slots_[slot].stateRevision != rollback_.readRevision(slot))
			return Status::SourceUnavailable;
		const auto& target = sessions_[slots_[slot].cursor];
		if (!target.identity || !target.complete || !target.changed)
			return Status::SourceUnavailable;
		const auto currentLength = pool_.length(pool_.sealedHandle(slot));
		if (!currentLength)
			return Status::SourceUnavailable;
		if (!pool_.canBeginDetached(slot, currentLength))
			return Status::NoMemory;
	}
	if (restoreSequence_ == std::numeric_limits<Generation>::max())
		return Status::Unavailable;
	const auto source = pool_.sealedHandle(slot);
	const auto length = pool_.length(source);
	if (!length)
		return Status::Stale;
	RestoreJob job{};
	job.id = {++restoreSequence_};
	job.slot = slot;
	job.source = source;
	job.length = length;
	job.revision = rollback_.readRevision(slot);
	job.redo = redo;
	const auto sessionIndex =
	    redo ? (slots_[slot].cursor == none ? slots_[slot].head : sessions_[slots_[slot].cursor].next)
	         : slots_[slot].cursor;
	if (sessionIndex == none)
		return Status::SourceUnavailable;
	auto& session = sessions_[sessionIndex];
	job.sessionIdentity = session.identity;
	if (redo) {
		job.candidate = session.afterVersion;
		job.reusedVersion = true;
	}
	else
		job.candidateNeeded = true;
	job.active = true;
	restore_ = job;
	id = job.id;
	return Status::Ok;
}

Status History::reserveRestore(RestoreId id, uint32_t maxPages) {
	if (!restore_.active || !id.valid() || id.generation != restore_.id.generation)
		return Status::Stale;
	auto& job = restore_;
	if (job.reusedVersion)
		return Status::Ok;
	if (job.candidateNeeded) {
		PhraseHandle candidate{};
		const auto allocated = pool_.beginDetached(job.slot, job.length, candidate);
		if (allocated != Status::Ok)
			return allocated;
		job.candidate = candidate;
		job.candidateNeeded = false;
	}
	return pool_.reserveDetachedPages(job.candidate, maxPages);
}

Status History::serviceRestore(RestoreId id, uint32_t maxFrames) {
	if (!restore_.active || !id.valid() || id.generation != restore_.id.generation)
		return Status::Stale;
	auto& job = restore_;
	if (pool_.length(job.source) != job.length || rollback_.readRevision(job.slot) != job.revision
	    || rollback_.provisional(job.slot) || rollback_.pending(job.slot)) {
		cancelRestore(job.slot);
		return Status::Stale;
	}
	if (job.reusedVersion)
		return Status::Ok;
	if (maxFrames == 0)
		return Status::Busy;
	if (job.candidateNeeded || !pool_.detachedReservationComplete(job.candidate))
		return Status::Busy;
	unsigned sessionId = none;
	for (unsigned i = 0; i < kMaxSessions; ++i)
		if (sessions_[i].identity == job.sessionIdentity) {
			sessionId = i;
			break;
		}
	if (sessionId == none || !sessions_[sessionId].complete) {
		cancelRestore(job.slot);
		return Status::Stale;
	}
	constexpr FrameIndex perPage = kPageBytes / sizeof(Frame);
	const auto end = std::min<FrameIndex>(job.length, job.cursor + std::min<uint32_t>(maxFrames, 128));
	builderActive_ = true;
	struct BuilderGuard {
		bool& active;
		~BuilderGuard() { active = false; }
	} builderGuard{builderActive_};
	for (; job.cursor < end; ++job.cursor) {
		if (job.cancelPending)
			return Status::Stale;
		const auto* source = pool_.frame(job.source, job.cursor);
		if (!source) {
			cancelRestore(job.slot);
			return Status::Stale;
		}
		Frame value = *source;
		const auto entry = pages_.find(job.sessionIdentity, job.cursor / perPage);
		if (entry != detail::BeforeImages::none)
			value = pages_.data(entry)[job.cursor % perPage];
		const auto status = pool_.writeDetached(job.candidate, job.cursor, value);
		if (status != Status::Ok) {
			cancelRestore(job.slot);
			return status;
		}
	}
	if (job.cursor != job.length)
		return Status::Busy;
	const auto status = pool_.sealDetached(job.candidate);
	if (status != Status::Ok) {
		cancelRestore(job.slot);
		return status;
	}
	return Status::Ok;
}

Status History::commitRestore(RestoreId id) {
	if (!restore_.active || !id.valid() || id.generation != restore_.id.generation)
		return Status::Stale;
	auto job = restore_;
	if (pool_.length(job.source) != job.length || pool_.length(job.candidate) != job.length
	    || rollback_.readRevision(job.slot) != job.revision || rollback_.provisional(job.slot)
	    || rollback_.pending(job.slot)) {
		cancelRestore(job.slot);
		return Status::Stale;
	}
	unsigned sessionIndex = none;
	for (unsigned i = 0; i < kMaxSessions; ++i)
		if (sessions_[i].identity == job.sessionIdentity) {
			sessionIndex = i;
			break;
		}
	if (sessionIndex == none) {
		cancelRestore(job.slot);
		return Status::Stale;
	}
	auto& slot = slots_[job.slot];
	auto& session = sessions_[sessionIndex];
	const auto expectedSession =
	    job.redo ? (slot.cursor == none ? slot.head : sessions_[slot.cursor].next) : slot.cursor;
	if (expectedSession != sessionIndex || session.identity != job.sessionIdentity
	    || (job.redo && session.afterVersion.generation != job.candidate.generation)
	    || (!job.redo && (!session.complete || !session.changed || session.active))) {
		cancelRestore(job.slot);
		return Status::Stale;
	}
	PhraseHandle previous{};
	const auto status = pool_.replace(job.candidate, previous);
	if (status != Status::Ok) {
		cancelRestore(job.slot);
		return status;
	}
	if (job.redo) {
		session.afterVersion = {};
		if (previous.valid())
			pool_.retire(previous);
		slot.cursor = sessionIndex;
	}
	else {
		session.afterVersion = previous;
		slot.cursor = session.previous;
	}
	slot.stateRevision = rollback_.readRevision(job.slot);
	restore_ = {};
	return Status::Ok;
}

void History::cancelRestore(SlotId slot) {
	if (!restore_.active || (validSlot(slot) && restore_.slot != slot))
		return;
	if (builderActive_) {
		restore_.cancelPending = true;
		return;
	}
	if (!restore_.reusedVersion && restore_.candidate.valid())
		pool_.retire(restore_.candidate);
	restore_ = {};
}

void History::invalidateRedo(SlotId slot) {
	if (!validSlot(slot))
		return;
	auto branch = slots_[slot].cursor == none ? slots_[slot].head : sessions_[slots_[slot].cursor].next;
	if (branch == none)
		return;
	if (slots_[slot].cursor == none)
		slots_[slot].head = slots_[slot].tail = slots_[slot].latest = none;
	else {
		sessions_[slots_[slot].cursor].next = none;
		slots_[slot].tail = slots_[slot].latest = slots_[slot].cursor;
	}
	enqueueRetirement(branch);
}

void History::mappingReplaced(SlotId slot) {
	if (validSlot(slot))
		slots_[slot].stateRevision = rollback_.readRevision(slot);
}

void History::resetSlot(SlotId slot) {
	if (!validSlot(slot))
		return;
	cancelRestore(slot);
	if (slots_[slot].active != none) {
		const auto active = slots_[slot].active;
		sessions_[active].previous = sessions_[active].next = none;
		enqueueRetirement(active);
	}
	if (slots_[slot].head != none)
		enqueueRetirement(slots_[slot].head);
	slots_[slot] = {};
}

uint32_t History::serviceRetirement(uint32_t maxEntries) {
	if (restore_.active && restore_.cancelPending && !builderActive_) {
		if (restore_.candidate.valid() && !restore_.reusedVersion)
			pool_.retire(restore_.candidate);
		restore_ = {};
	}
	uint32_t processed = 0;
	// Incomplete active boundaries keep their descriptor but can release any
	// already-captured before-image entries without walking them on the audio path.
	for (auto& session : sessions_) {
		if (!session.identity || !session.cleanupQueued || processed >= maxEntries)
			continue;
		processed += pages_.eraseStep(session.identity, session.retirementScan, maxEntries - processed);
		if (session.retirementScan == pages_.capacity())
			session.cleanupQueued = false;
	}
	for (auto& retirement : retirements_) {
		if (!retirement.active || processed >= maxEntries)
			continue;
		const auto id = retirement.current;
		if (id == none || id >= kMaxSessions || sessions_[id].identity != retirement.identity) {
			retirement = {};
			continue;
		}
		auto& session = sessions_[id];
		session.retiring = true;
		if (session.afterVersion.valid()) {
			pool_.retire(session.afterVersion);
			session.afterVersion = {};
		}
		processed += pages_.eraseStep(session.identity, session.retirementScan, maxEntries - processed);
		if (session.retirementScan == pages_.capacity()) {
			const auto next = session.next;
			const auto nextIdentity = next == none ? 0 : sessions_[next].identity;
			session = {};
			retirement.current = next;
			retirement.identity = nextIdentity;
			if (next == none)
				retirement = {};
		}
		break;
	}
	return processed;
}

bool History::retirementPending() const {
	for (const auto& session : sessions_)
		if (session.cleanupQueued)
			return true;
	for (const auto& retirement : retirements_)
		if (retirement.active)
			return true;
	return false;
}
} // namespace loop_station
