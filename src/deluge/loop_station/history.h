#pragma once
#include "before_images.h"
namespace loop_station {
// Serialized audio-owner operations. Best effort only: this arena never owns
// mandatory gesture protection and cannot change capture or playback gates.
class History {
public:
	static constexpr unsigned kMaxSessions = 64;
	// Candidate only, outside active memory. Task 8 must qualify the total budget.
	static constexpr size_t kCandidateBytes = 4 * 1024 * 1024;
	History(PagePool& pool, GestureRollback& rollback) : pool_(pool), rollback_(rollback) {}
	Status initialize(std::span<std::byte> storage);
	void beginSession(SlotId slot, Generation generation);
	// Called at successful new-initial-capture admission, before completion:
	// opening a fresh base recording invalidates any prior volatile chain and
	// Redo branch, independent of the eventual frame count. A failed admission
	// never reaches this.
	void beginInitialEdit(SlotId slot);
	void beforeWrite(SlotId slot, FrameIndex index, Frame original);
	void endSession(SlotId slot);
	// Register a completed, nonzero base recording as the initial boundary of a
	// fresh chain. Detaches nothing; the caller has already sealed and bound.
	void recordInitial(SlotId slot, PhraseHandle handle);
	bool activeSessionChanged(SlotId slot) const;
	bool activeSessionComplete(SlotId slot) const;
	bool canAdmitInitialBoundary() const;
	bool canUndo(SlotId slot) const;
	bool canRedo(SlotId slot) const;
	Status requestRestore(SlotId slot, bool redo, RestoreId& id);
	Status serviceRestore(RestoreId id, uint32_t maxFrames);
	Status reserveRestore(RestoreId id, uint32_t maxPages);
	Status commitRestore(RestoreId id);
	void cancelRestore(SlotId slot);
	void invalidateRedo(SlotId slot);
	void mappingReplaced(SlotId slot);
	void resetSlot(SlotId slot);
	uint32_t serviceRetirement(uint32_t maxEntries);
	bool restorePending() const { return restore_.active; }
	bool retirementPending() const;
	uint32_t retainedPages() const { return pages_.count(); }
	unsigned retainedSessions() const;
	const detail::BeforeImages::Work& work() const { return pages_.work(); }
	uint64_t pressure() const { return pressure_; }

private:
	static constexpr unsigned none = kMaxSessions;
	struct Session {
		PhraseHandle handle{};
		PhraseHandle afterVersion{};
		Generation identity = 0, revision = 0;
		uint32_t pages = 0, lastPage = UINT32_MAX, retirementScan = 0;
		unsigned previous = none, next = none;
		bool active = false, changed = false, complete = true, retiring = false, cleanupQueued = false;
		bool initial = false; // Base recording: Undo detaches, Redo republishes afterVersion.
	};
	void discard(unsigned session);
	void losePages(unsigned session);
	void enqueueRetirement(unsigned root);
	// Shared bounded reclaim policy for descriptor/page pressure: retire exactly
	// one boundary. `reclaimOldestBoundary` is the admission-path selection;
	// `reclaimBoundary` applies the policy to a chosen boundary root.
	void reclaimOldestBoundary();
	void reclaimBoundary(unsigned root);
	// Retire a boundary together with every dependent successor so a later Redo
	// cannot skip the removed step. Used for interior boundaries.
	void fenceForward(unsigned root);
	struct Retirement {
		unsigned current = none;
		Generation identity = 0;
		bool active = false;
	};
	PagePool& pool_;
	GestureRollback& rollback_;
	detail::BeforeImages pages_;
	Session sessions_[kMaxSessions]{};
	Retirement retirements_[kMaxSessions]{};
	struct Slot {
		unsigned active = none, latest = none, head = none, tail = none, cursor = none;
		Generation stateRevision = 0;
	};
	Slot slots_[kTrackCount]{};
	Generation sequence_ = 0;
	uint64_t pressure_ = 0;
	struct RestoreJob {
		RestoreId id{};
		PhraseHandle source{}, candidate{};
		Generation sessionIdentity = 0, revision = 0;
		FrameIndex cursor = 0, length = 0;
		SlotId slot = kInvalidSlot;
		bool active = false, redo = false, reusedVersion = false, candidateNeeded = false, cancelPending = false;
		// Reconstruction/commit-pending: once the before-image candidate has been
		// fully built and sealed, reserve/service are idempotent and a later commit
		// retry must not re-reserve the now-sealed record as if it were Stale.
		bool candidateReady = false;
		// Initial boundary Undo detaches the live recording instead of building a
		// before-image candidate. Redo from empty republishes afterVersion and has
		// no current source phrase.
		bool initialUndo = false, emptyCurrent = false;
	};
	RestoreJob restore_{};
	bool builderActive_ = false;
	Generation restoreSequence_ = 0;
};
} // namespace loop_station
