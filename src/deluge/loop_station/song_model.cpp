#include "song_model.h"
#include "invariant.h"
#include "menu_model.h"
#include <algorithm>
#include <limits>
namespace loop_station {
Generation SongModel::nextModelIdentity() {
	static Generation next = 0;
	if (next == std::numeric_limits<Generation>::max())
		return 0; // Exhaustion: navigation stays unavailable; identities never recycle.
	return ++next;
}
void SongModel::advanceTransportEpoch() {
	if (++transportEpoch_ == 0)
		++transportEpoch_;
}
void SongModel::finishHistoryNavigation(Status status, bool complete) {
	if (!navigation_.active)
		return;
	const auto result =
	    HistoryNavigationResult{navigation_.id, navigation_.slot, navigation_.redo,
	                            complete ? HistoryNavigationState::Complete : HistoryNavigationState::Failed, status};
	if (restoreOwner_.request && restoreOwner_.request == navigation_.id.generation)
		cancelRestore(navigation_.slot);
	navigation_ = {};
	navigationResult_ = result;
}
void SongModel::cancelHistoryNavigation(Status status) {
	if (!navigation_.active)
		return;
	const auto id = navigation_.id;
	const auto slot = navigation_.slot;
	const auto redo = navigation_.redo;
	{
		Boundary boundary(host_);
		if (restoreOwner_.request && restoreOwner_.request == id.generation) {
			history_.cancelRestore(slot);
			restoreOwner_ = {};
		}
	}
	navigation_ = {};
	navigationResult_ = {id, slot, redo, HistoryNavigationState::Failed, status};
}
SongModel::~SongModel() {
	requestRelease();
	detail::invariant(safeToRelease());
	// Destruction order: adapter/engine/audio/readers/pool, pin, reservation.
}
Status SongModel::start() {
	if (ready_ || releasing_ || inFlight_)
		return Status::Busy;
	const auto generation = generation_;
	++inFlight_;
	auto status = memory_.acquire(kConfiguredMemoryProfile);
	--inFlight_;
	if (status != Status::Ok)
		return status;
	if (generation != generation_ || releasing_) {
		memory_.release();
		return Status::Stale;
	}
	pin_.emplace(memory_.pin());
	status = pool_.initialize(memory_.activeStorage());
	if (status == Status::Ok)
		status = rollback_.initialize(memory_.gestureStorage());
	if (status == Status::Ok)
		status = history_.initialize(memory_.historyStorage());
	if (status == Status::Ok)
		status = snapshots_.initialize(memory_.snapshotStorage());
	for (SlotId slot = 0; status == Status::Ok && slot < kTrackCount; ++slot)
		status = transpose_.prepare(slot);
	if (status != Status::Ok) {
		pin_.reset();
		memory_.release();
		return status;
	}
	audio_.setWriteObservers(&history_, &snapshots_);
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		transpose_.setCents(slot, engine_.settings().tracks[slot].transposeCents);
	Boundary boundary(host_);
	ready_ = registered_ = true;
	host_.registerAudio(&adapter_, &clock_);
	return Status::Ok;
}
bool SongModel::replacing(SlotId slot) const {
	return validSlot(slot) && (settingUp_[slot] || engine_.state(slot).action == PendingAction::Clear);
}
Status SongModel::ensureBound(SlotId slot) {
	if (!validSlot(slot))
		return Status::Invalid;
	if (!ready())
		return Status::Unavailable;
	if (replacing(slot))
		return Status::Busy;
	if (bindings_[slot])
		return Status::Ok;
	const auto generation = generation_;
	const auto slotGeneration = slotGenerations_[slot];
	settingUp_[slot] = true;
	++inFlight_;
	auto* candidate = host_.allocateClip();
	pending_[slot] = candidate;
	auto status = candidate ? host_.prepareClip(candidate, slot) : Status::NoMemory;
	{
		Boundary boundary(host_);
		// Compare identity before dereferencing: a yielding prepare may delete it.
		const bool alive = candidate && pending_[slot] == candidate;
		if (generation != generation_ || slotGeneration != slotGenerations_[slot] || !ready() || (candidate && !alive))
			status = Status::Stale;
		if (status == Status::Ok) {
			status = host_.attach(candidate, &adapter_, slot);
			if (status == Status::Ok) {
				status = host_.publishClip(candidate);
				if (status == Status::Ok)
					bindings_[slot] = candidate;
				if (status == Status::Ok)
					explicitlyUnbound_[slot] = false;
				if (status == Status::Ok)
					transpose_.reset(slot);
				else
					host_.attach(candidate, nullptr, kInvalidSlot);
			}
		}
		pending_[slot] = nullptr;
		if (status != Status::Ok && alive)
			host_.discardClip(candidate);
		settingUp_[slot] = false;
		--inFlight_;
	}
	return status;
}
Status SongModel::bindExisting(SlotId slot, AudioClip* clip) {
	if (!validSlot(slot) || !clip)
		return Status::Invalid;
	if (!ready())
		return Status::Unavailable;
	Boundary boundary(host_);
	if (replacing(slot))
		return Status::Busy;
	if (bindings_[slot])
		return bindings_[slot] == clip ? Status::Ok : Status::Busy;
	for (auto* bound : bindings_)
		if (bound == clip)
			return Status::Busy;
	for (auto* candidate : pending_)
		if (candidate == clip)
			return Status::Busy;
	const auto status = host_.attach(clip, &adapter_, slot);
	if (status == Status::Ok) {
		bindings_[slot] = clip;
		explicitlyUnbound_[slot] = false;
		transpose_.reset(slot);
	}
	return status;
}
Status SongModel::unbind(SlotId slot) {
	if (!validSlot(slot))
		return Status::Invalid;
	Boundary boundary(host_);
	cancelRestore(slot);
	if (navigation_.active && navigation_.slot == slot)
		cancelHistoryNavigation(Status::Stale);
	history_.resetSlot(slot);
	snapshots_.invalidateSlot(slot);
	++slotGenerations_[slot]; // Other slots' setup transactions remain independent.
	archive_.cancel(slot);
	transpose_.reset(slot);
	if (bindings_[slot]) {
		host_.attach(bindings_[slot], nullptr, kInvalidSlot);
		explicitlyUnbound_[slot] = true;
	}
	bindings_[slot] = nullptr;
	if (ready_)
		engine_.removeSlot(slot, host_.now(), clock_);
	return Status::Ok;
}
void SongModel::clipDeleted(AudioClip* clip) {
	if (!clip)
		return;
	Boundary boundary(host_);
	for (SlotId id = 0; id < kTrackCount; ++id) {
		if (pending_[id] == clip) {
			pending_[id] = nullptr;
			++slotGenerations_[id];
		}
		if (bindings_[id] == clip)
			unbind(id);
	}
}
void SongModel::requestExit() {
	(void)requestViewExit();
}
Status SongModel::requestTransportStop() {
	if (!ready()) {
		if (inFlight_) {
			++generation_;
			return Status::Busy;
		}
		return Status::Unavailable;
	}
	// Engine::TransportStop closes capture gates before servicing the immediate
	// stop action. This is intentionally independent of the stop quantization.
	return dispatch({CommandType::TransportStop});
}
Status SongModel::requestViewExit() {
	if (!ready()) {
		if (inFlight_) {
			++generation_;
			return Status::Busy;
		}
		return Status::Unavailable;
	}
	Boundary boundary(host_);
	++generation_;
	// A root-view handoff is a supported transition, not model replacement: an
	// accepted restore/navigation job keeps running (its own ownership epoch is
	// unchanged). The generation bump still invalidates in-flight setup transactions.
	return engine_.dispatch({CommandType::Exit}, host_.now(), clock_);
}
void SongModel::cancelHeldPadGestures() {
	if (!ready())
		return;
	Boundary boundary(host_);
	engine_.cancelHeldGestures();
}
bool SongModel::hasUnsavedAudio() const {
	if (!ready_)
		return false;
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		const auto state = engine_.state(slot);
		if (state.capture || (state.pending && state.action == PendingAction::Initial)
		    || (hasSavedRevision_[slot] && audio_.contentRevision(slot) != savedRevisions_[slot])
		    || (!hasSavedRevision_[slot] && state.hasPhrase))
			return true;
	}
	return false;
}
void SongModel::markAudioSaved(SlotId slot, Generation revision) {
	if (!validSlot(slot))
		return;
	savedRevisions_[slot] = revision;
	hasSavedRevision_[slot] = true;
}
uint8_t SongModel::captureActivityMask() const {
	uint8_t mask = 0;
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		const auto state = engine_.state(slot);
		if (state.capture || (state.pending && state.action == PendingAction::Initial))
			mask |= uint8_t(1u << slot);
	}
	return mask;
}
Status SongModel::requestSongReplacement(bool discardUnsaved) {
	if (!ready_) {
		if (inFlight_) {
			requestRelease();
			return safeToRelease() ? Status::Ok : Status::Busy;
		}
		return releasing_ && !safeToRelease() ? Status::Busy : Status::Ok;
	}
	if (releasing_)
		return safeToRelease() ? Status::Ok : Status::Busy;
	Boundary boundary(host_);
	if (hasUnsavedAudio() && !discardUnsaved)
		return Status::Cancelled;
	requestRelease();
	return safeToRelease() ? Status::Ok : Status::Busy;
}
Status SongModel::requestSongReplacement(bool discardUnsaved,
                                         const std::array<Generation, kTrackCount>& confirmedRevisions,
                                         uint8_t confirmedCaptureMask) {
	if (!ready_) {
		if (inFlight_) {
			requestRelease();
			return safeToRelease() ? Status::Ok : Status::Busy;
		}
		return releasing_ && !safeToRelease() ? Status::Busy : Status::Ok;
	}
	if (releasing_)
		return safeToRelease() ? Status::Ok : Status::Busy;
	Boundary boundary(host_);
	const bool unsaved = hasUnsavedAudio();
	if (unsaved && !discardUnsaved)
		return Status::Cancelled;
	if (discardUnsaved) {
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			if (audio_.contentRevision(slot) != confirmedRevisions[slot])
				return Status::Cancelled;
		if (captureActivityMask() != confirmedCaptureMask)
			return Status::Cancelled;
	}
	requestRelease();
	return safeToRelease() ? Status::Ok : Status::Busy;
}
Status SongModel::requestArchive(SlotId slot) {
	if (!ready())
		return Status::Unavailable;
	if (!validSlot(slot))
		return Status::Invalid;
	Boundary boundary(host_);
	if (!bindings_[slot])
		return Status::Unavailable;
	if (replacing(slot))
		return Status::Busy;
	const auto handle = pool_.sealedHandle(slot);
	return handle.valid() ? archive_.request(slot, handle.generation) : Status::Stale;
}
Status SongModel::requestRestore(SlotId slot, bool redo, RestoreId& id) {
	id = {};
	if (!ready())
		return Status::Unavailable;
	if (!validSlot(slot))
		return Status::Invalid;
	Boundary boundary(host_);
	if (restoreOwner_.request)
		return Status::Busy;
	const auto state = engine_.state(slot);
	if (!bindings_[slot] || !state.hasPhrase)
		return Status::SourceUnavailable;
	if (state.capture || state.pending || state.action != PendingAction::None || replacing(slot))
		return Status::Busy;
	RestoreId historyId{};
	const auto status = history_.requestRestore(slot, redo, historyId);
	if (status == Status::Ok) {
		if (restoreSequence_ == std::numeric_limits<Generation>::max()) {
			history_.cancelRestore(slot);
			return Status::Unavailable;
		}
		id = {++restoreSequence_};
		restoreOwner_ = {};
		restoreOwner_.slot = slot;
		restoreOwner_.request = id.generation;
		restoreOwner_.slotGeneration = slotGenerations_[slot];
		restoreOwner_.modelGeneration = modelEpoch_;
		restoreOwner_.historyId = historyId;
	}
	return status;
}

Status SongModel::historyTarget(SlotId slot, bool redo, HistoryTarget& target) const {
	target = {};
	if (!ready())
		return Status::Unavailable;
	if (!validSlot(slot))
		return Status::Invalid;
	target.slot = slot;
	target.redo = redo;
	target.modelIdentity = modelIdentity_;
	target.slotGeneration = slotGenerations_[slot];
	target.historyEpoch = engine_.historyEpoch(slot);
	target.transportEpoch = transportEpoch_;
	if (!modelIdentity_)
		return Status::Unavailable;
	if (!bindings_[slot])
		return Status::SourceUnavailable;
	if (replacing(slot))
		return Status::Busy;
	const auto state = engine_.state(slot);
	if (redo) {
		// Redo while capturing is unavailable; a new recording invalidates the branch.
		if (state.capture || state.pending)
			return Status::SourceUnavailable;
		return history_.canRedo(slot) ? Status::Ok : Status::SourceUnavailable;
	}
	// An active initial/overdub may be armed: confirmation closes it and reports
	// honestly if no layer resulted. Known-unrestorable overdubs are not armable.
	if (state.capture) {
		if (!state.playing)
			return Status::Ok; // Active initial: closure decides (may be zero-frame).
		if (!history_.activeSessionChanged(slot))
			return Status::Ok; // Unchanged: confirmation closes and reports no layer.
		if (!history_.activeSessionComplete(slot))
			return Status::SourceUnavailable; // Known-incomplete before-images.
		if (!pool_.canBeginDetached(slot, state.phraseFrames))
			return Status::NoMemory; // Known detached-admission failure.
		return Status::Ok;
	}
	if (state.pending)
		return Status::SourceUnavailable;
	return history_.canUndo(slot) ? Status::Ok : Status::SourceUnavailable;
}

bool SongModel::historyTargetCurrent(const HistoryTarget& target) const {
	if (!target.modelIdentity || target.modelIdentity != modelIdentity_)
		return false;
	if (!ready() || !validSlot(target.slot))
		return false;
	if (!bindings_[target.slot])
		return false;
	if (target.slotGeneration != slotGenerations_[target.slot])
		return false;
	if (target.historyEpoch != engine_.historyEpoch(target.slot))
		return false;
	if (target.transportEpoch != transportEpoch_)
		return false;
	// Added frames within one capture session and a changing free-page count are
	// deliberately not part of identity.
	return true;
}

Status SongModel::requestHistoryNavigation(const HistoryTarget& target, RestoreId& id) {
	id = {};
	if (!ready())
		return Status::Unavailable;
	if (!validSlot(target.slot))
		return Status::Invalid;
	Boundary boundary(host_);
	if (navigation_.active || restoreOwner_.request)
		return Status::Busy; // Reject repeated requests; never queue button presses.
	if (!historyTargetCurrent(target))
		return Status::Stale;
	if (restoreSequence_ == std::numeric_limits<Generation>::max())
		return Status::Unavailable;
	const auto slot = target.slot;
	const auto state = engine_.state(slot);
	if (target.redo && state.capture)
		return Status::SourceUnavailable; // Redo while capturing is unavailable.
	const bool activeCapture = !target.redo && state.capture;
	const bool activeOverdub = activeCapture && state.playing;
	const bool overdubChanged = activeOverdub && history_.activeSessionChanged(slot);
	if (activeOverdub && overdubChanged) {
		// A known-unrestorable changed overdub must not stop the recording. These
		// conditions are observable under the boundary before any closure.
		if (!history_.activeSessionComplete(slot))
			return Status::SourceUnavailable;
		if (!pool_.canBeginDetached(slot, state.phraseFrames))
			return Status::NoMemory;
	}
	if (activeCapture) {
		// Recheck the same target immediately before closing; a change during any
		// earlier work must leave capture running rather than close a different session.
		if (!historyTargetCurrent(target))
			return Status::Stale;
		const auto closed = engine_.closeCaptureForHistory(slot);
		if (closed == Status::Busy)
			return Status::Stale; // Capture vanished; never target an older boundary.
		if (closed != Status::Ok)
			return closed;
	}
	if (activeOverdub && !overdubChanged) {
		// The just-closed gate contributed no layer. Report honestly instead of
		// falling through to an older boundary; the capture stays closed.
		return Status::SourceUnavailable;
	}
	id = {++restoreSequence_};
	navigation_ = {};
	navigation_.active = true;
	navigation_.id = id;
	navigation_.slot = slot;
	navigation_.redo = target.redo;
	navigation_.modelGeneration = modelEpoch_;
	navigation_.slotGeneration = slotGenerations_[slot];
	restoreOwner_ = {};
	restoreOwner_.slot = slot;
	restoreOwner_.request = id.generation;
	restoreOwner_.slotGeneration = slotGenerations_[slot];
	restoreOwner_.modelGeneration = modelEpoch_;
	navigationResult_ = {id, slot, target.redo, HistoryNavigationState::Applying, Status::Ok};
	return Status::Ok;
}

void SongModel::serviceHistoryNavigation(uint32_t maxFrames) {
	if (!navigation_.active)
		return;
	if (navigationServicing_ || restoreServicing_ || restoreIoServicing_)
		return; // Never recursively service or commit another request.
	navigationServicing_ = true;
	struct NavigationGuard {
		bool& active;
		~NavigationGuard() { active = false; }
	} guard{navigationServicing_};
	const auto slot = navigation_.slot;
	if (!ready() || navigation_.modelGeneration != modelEpoch_ || !validSlot(slot)
	    || navigation_.slotGeneration != slotGenerations_[slot] || !bindings_[slot]) {
		finishHistoryNavigation(Status::Stale, false);
		return;
	}
	if (!restoreOwner_.request || restoreOwner_.request != navigation_.id.generation) {
		// Cancelled by transport, rebinding, replacement or release.
		finishHistoryNavigation(Status::Stale, false);
		return;
	}
	if (!navigation_.admitted) {
		Status admitted;
		{
			Boundary boundary(host_);
			// A confirmed capture closure leaves pending rollback entries that fence
			// History admission. Retire them with bounded foreground work only.
			rollback_.service(64);
			if (rollback_.provisional(slot) || rollback_.pending(slot))
				return; // Wait for bounded retirement; keep the job and its ID.
			RestoreId historyId{};
			admitted = history_.requestRestore(slot, navigation_.redo, historyId);
			if (admitted == Status::Ok) {
				restoreOwner_.historyId = historyId;
				navigation_.admitted = true;
			}
		}
		if (admitted == Status::Busy)
			return;
		if (admitted != Status::Ok) {
			finishHistoryNavigation(admitted, false);
			return;
		}
	}
	const auto serviced = serviceRestore(navigation_.id, maxFrames);
	if (serviced == Status::Busy)
		return; // Retain the job and its ID; the foreground owner retries.
	if (serviced != Status::Ok) {
		finishHistoryNavigation(serviced, false);
		return;
	}
	const auto committed = commitRestore(navigation_.id);
	if (committed == Status::Busy)
		return; // Retain on commit Busy; do not drop the ID or claim success.
	if (committed != Status::Ok) {
		finishHistoryNavigation(committed, false);
		return;
	}
	finishHistoryNavigation(Status::Ok, true);
}

Status SongModel::requestCheckpointRestore(SlotId slot, Generation checkpointIdentity, RestoreId& id) {
	id = {};
	if (!ready())
		return Status::Unavailable;
	if (!validSlot(slot) || !checkpointIdentity)
		return Status::Invalid;
	const auto slotGeneration = slotGenerations_[slot];
	// Accepted checkpoint work uses the ownership epoch (like user jobs), not the
	// volatile setup generation: a supported root-view handoff must not abandon an
	// accepted restore. Genuine edits/rebinds/teardown cancel through slot
	// generation, source revision and release instead.
	const auto modelGeneration = modelEpoch_;
	{
		Boundary boundary(host_);
		const auto state = engine_.state(slot);
		if (restoreOwner_.request)
			return Status::Busy;
		if (!bindings_[slot])
			return Status::SourceUnavailable;
		if (state.capture || state.pending || state.action != PendingAction::None || replacing(slot)
		    || rollback_.provisional(slot) || rollback_.pending(slot))
			return Status::Busy;
		const auto source = pool_.sealedHandle(slot);
		const auto sourceRevision = rollback_.readRevision(slot);
		if (restoreSequence_ == std::numeric_limits<Generation>::max())
			return Status::Unavailable;
		id = {++restoreSequence_};
		restoreOwner_ = {};
		restoreOwner_.slot = slot;
		restoreOwner_.request = id.generation;
		restoreOwner_.slotGeneration = slotGeneration;
		restoreOwner_.modelGeneration = modelGeneration;
		restoreOwner_.checkpointIdentity = checkpointIdentity;
		restoreOwner_.checkpoint = true;
		restoreOwner_.validatingHeader = true;
		restoreOwner_.source = source;
		restoreOwner_.sourceRevision = sourceRevision;
	}
	id = {restoreOwner_.request};
	return Status::Ok;
}

Status SongModel::serviceRestore(RestoreId id, uint32_t maxFrames) {
	if (!restoreOwner_.request || restoreOwner_.request != id.generation || !id.valid())
		return Status::Stale;
	if (restoreServicing_)
		return Status::Busy;
	restoreServicing_ = true;
	struct ServiceGuard {
		bool& active;
		~ServiceGuard() { active = false; }
	} serviceGuard{restoreServicing_};
	++inFlight_;
	if (restoreOwner_.canceled) {
		--inFlight_;
		return Status::Stale;
	}
	if (restoreOwner_.slotGeneration != slotGenerations_[restoreOwner_.slot]
	    || restoreOwner_.modelGeneration != modelEpoch_ || !ready()) {
		cancelRestore(restoreOwner_.slot);
		--inFlight_;
		return Status::Stale;
	}
	if (restoreOwner_.checkpoint) {
		const auto currentSource = pool_.sealedHandle(restoreOwner_.slot);
		if (restoreOwner_.source.slot != currentSource.slot
		    || restoreOwner_.source.generation != currentSource.generation
		    || restoreOwner_.sourceRevision != rollback_.readRevision(restoreOwner_.slot)) {
			cancelRestore(restoreOwner_.slot);
			--inFlight_;
			return Status::Stale;
		}
		if (!restoreOwner_.ioReady) {
			--inFlight_;
			return restoreOwner_.readerFinished ? Status::Ok : Status::Busy;
		}
		const auto status = restoreOwner_.ioStatus;
		if (restoreOwner_.ioClose) {
			restoreOwner_.ioReady = false;
			if (status != Status::Ok) {
				cancelRestore(restoreOwner_.slot);
				--inFlight_;
				return status;
			}
			restoreOwner_.readerFinished = true;
			--inFlight_;
			return Status::Ok;
		}
		if (status != Status::Ok && status != Status::Busy) {
			cancelRestore(restoreOwner_.slot);
			--inFlight_;
			return status;
		}
		if (restoreOwner_.validatingHeader) {
			if (status == Status::Ok) {
				if (!restoreOwner_.ioTotalFrames) {
					cancelRestore(restoreOwner_.slot);
					--inFlight_;
					return Status::Invalid;
				}
				const auto state = engine_.state(restoreOwner_.slot);
				if (state.capture || state.pending || state.action != PendingAction::None
				    || replacing(restoreOwner_.slot) || rollback_.provisional(restoreOwner_.slot)
				    || rollback_.pending(restoreOwner_.slot)) {
					cancelRestore(restoreOwner_.slot);
					--inFlight_;
					return Status::Busy;
				}
				PhraseHandle candidate{};
				Status reserved;
				{
					Boundary boundary(host_);
					reserved = restoreOwner_.canceled
					               ? Status::Stale
					               : pool_.beginDetached(restoreOwner_.slot, restoreOwner_.ioTotalFrames, candidate);
				}
				if (reserved != Status::Ok) {
					cancelRestore(restoreOwner_.slot);
					--inFlight_;
					return reserved;
				}
				restoreOwner_.candidate = candidate;
				restoreOwner_.totalFrames = restoreOwner_.ioTotalFrames;
				restoreOwner_.validatingHeader = false;
			}
			restoreOwner_.ioReady = false;
			--inFlight_;
			return Status::Busy;
		}
		if (restoreOwner_.ioTotalFrames != restoreOwner_.totalFrames
		    || restoreOwner_.ioFramesRead > restoreFrames_.size()) {
			cancelRestore(restoreOwner_.slot);
			--inFlight_;
			return Status::Stale;
		}
		if (!maxFrames) {
			--inFlight_;
			return Status::Busy;
		}
		Status reservation;
		{
			Boundary boundary(host_);
			reservation = pool_.reserveDetachedPages(restoreOwner_.candidate, 8);
		}
		if (reservation == Status::NoMemory || reservation == Status::Stale) {
			cancelRestore(restoreOwner_.slot);
			--inFlight_;
			return reservation;
		}
		if (reservation == Status::Busy) {
			--inFlight_;
			return Status::Busy;
		}
		const auto copyCount =
		    std::min<size_t>({maxFrames, 128, restoreOwner_.ioFramesRead - restoreOwner_.ioFrameOffset});
		{
			Boundary boundary(host_);
			if (restoreOwner_.canceled) {
				--inFlight_;
				return Status::Stale;
			}
			for (size_t i = 0; i < copyCount; ++i) {
				const auto written = pool_.writeDetached(restoreOwner_.candidate, restoreOwner_.cursor,
				                                         restoreFrames_[restoreOwner_.ioFrameOffset + i]);
				if (written != Status::Ok) {
					cancelRestore(restoreOwner_.slot);
					--inFlight_;
					return written;
				}
				++restoreOwner_.cursor;
			}
			restoreOwner_.ioFrameOffset += copyCount;
			if (restoreOwner_.ioFrameOffset == restoreOwner_.ioFramesRead) {
				restoreOwner_.ioReady = false;
				restoreOwner_.ioFrameOffset = 0;
			}
			if (restoreOwner_.cursor == restoreOwner_.totalFrames) {
				const auto sealed = pool_.sealDetached(restoreOwner_.candidate);
				--inFlight_;
				return sealed == Status::Ok ? Status::Busy : sealed;
			}
		}
		if (!restoreOwner_.ioFramesRead && status == Status::Ok) {
			cancelRestore(restoreOwner_.slot);
			--inFlight_;
			return Status::StorageError;
		}
		--inFlight_;
		return Status::Busy;
	}
	{
		Boundary boundary(host_);
		const auto reserved = history_.reserveRestore(restoreOwner_.historyId, 8);
		if (reserved == Status::NoMemory || reserved == Status::Stale) {
			history_.cancelRestore(restoreOwner_.slot);
			restoreOwner_ = {};
			--inFlight_;
			return reserved;
		}
		if (reserved == Status::Busy) {
			--inFlight_;
			return Status::Busy;
		}
	}
	Status status;
	{
		Boundary boundary(host_);
		status = history_.serviceRestore(restoreOwner_.historyId, maxFrames);
	}
	if (status != Status::Busy && status != Status::Ok) {
		history_.cancelRestore(restoreOwner_.slot);
		restoreOwner_.canceled = true;
	}
	--inFlight_;
	return status;
}

Status SongModel::commitRestore(RestoreId id) {
	if (!restoreOwner_.request || restoreOwner_.request != id.generation || !id.valid())
		return Status::Stale;
	const auto slot = restoreOwner_.slot;
	Boundary boundary(host_);
	if (restoreOwner_.slotGeneration != slotGenerations_[slot] || restoreOwner_.modelGeneration != modelEpoch_
	    || !ready() || !bindings_[slot]) {
		if (restoreOwner_.checkpoint && restoreOwner_.candidate.valid())
			pool_.retire(restoreOwner_.candidate);
		else
			history_.cancelRestore(slot);
		restoreOwner_ = {};
		return Status::Stale;
	}
	const auto oldState = engine_.state(slot);
	if (oldState.capture || oldState.pending || oldState.action != PendingAction::None || rollback_.provisional(slot)
	    || rollback_.pending(slot))
		return Status::Busy;
	if (restoreOwner_.checkpoint) {
		if (!restoreOwner_.readerFinished || restoreOwner_.validatingHeader || restoreOwner_.canceled
		    || pool_.length(restoreOwner_.candidate) != restoreOwner_.totalFrames)
			return Status::Busy;
		PhraseHandle previous{};
		const auto swapped = pool_.replace(restoreOwner_.candidate, previous);
		if (swapped != Status::Ok)
			return swapped;
		const auto current = pool_.sealedHandle(slot);
		const auto length = pool_.length(current);
		const auto rebound = audio_.replace(slot, current);
		if (rebound != Status::Ok)
			return Status::Stale;
		engine_.phraseReplaced(slot, length, audio_.playing(slot));
		transpose_.reset(slot);
		markAudioSaved(slot, audio_.contentRevision(slot));
		history_.resetSlot(slot);
		// A loaded checkpoint is the base recording of one fresh volatile history
		// chain: after the successful rebind, register exactly one undoable initial
		// boundary so it can be undone to empty and redone as a whole recording.
		// Nothing is registered when any earlier step failed.
		history_.recordInitial(slot, current);
		if (pool_.length(previous))
			pool_.retire(previous);
		restoreOwner_ = {};
		return Status::Ok;
	}
	// Freeze any snapshot acquired before this navigation (e.g. an in-flight save)
	// so it stays owned with its original frames across the provider swap.
	snapshots_.preserve(slot);
	const auto status = history_.commitRestore(restoreOwner_.historyId);
	if (status != Status::Ok) {
		if (status == Status::Stale)
			restoreOwner_ = {};
		return status;
	}
	const auto handle = pool_.sealedHandle(slot);
	if (!handle.valid() || !pool_.length(handle)) {
		// Initial Undo detached and retained the recording; the empty state is a
		// successful publication, not Stale. History owns the pool detach, so this
		// only forgets the provider, then refreshes the mapping revision and engine.
		const auto detached = audio_.detachForHistory(slot);
		if (detached != Status::Ok)
			return Status::Stale; // Internal breach after a committed pool detach.
		history_.mappingReplaced(slot);
		engine_.phraseRemoved(slot);
		transpose_.reset(slot);
		restoreOwner_ = {};
		return Status::Ok;
	}
	const auto length = pool_.length(handle);
	const auto rebound = audio_.replace(slot, handle);
	if (rebound != Status::Ok) {
		// Eligibility was checked before History's constant-time pool swap; this
		// indicates an internal invariant breach, not a recoverable I/O error.
		return Status::Stale;
	}
	engine_.phraseReplaced(slot, length, audio_.playing(slot));
	transpose_.reset(slot);
	restoreOwner_ = {};
	return Status::Ok;
}

void SongModel::cancelRestore(SlotId slot) {
	Boundary boundary(host_);
	if (restoreOwner_.request && (!validSlot(slot) || restoreOwner_.slot == slot)) {
		if (restoreServicing_) {
			restoreOwner_.canceled = true;
			if (restoreOwner_.checkpoint)
				archive_.cancelCheckpointRead(restoreOwner_.slot, restoreOwner_.checkpointIdentity);
			else
				history_.cancelRestore(restoreOwner_.slot);
			return;
		}
		if (restoreOwner_.checkpoint) {
			restoreOwner_.canceled = true;
			archive_.cancelCheckpointRead(restoreOwner_.slot, restoreOwner_.checkpointIdentity);
			if (archive_.checkpointReadIdle()) {
				if (restoreOwner_.candidate.valid())
					pool_.retire(restoreOwner_.candidate);
				restoreOwner_ = {};
			}
		}
		else {
			history_.cancelRestore(restoreOwner_.slot);
			restoreOwner_ = {};
		}
	}
}
void SongModel::serviceArchive(uint32_t maxBytes) {
	if (restoreIoServicing_)
		return;
	restoreIoServicing_ = true;
	struct IoGuard {
		bool& active;
		~IoGuard() { active = false; }
	} ioGuard{restoreIoServicing_};
	// This method runs in the RESOURCE_SD | RESOURCE_SD_ROUTINE task. Perform
	// exactly one archive, checkpoint-reader, or cleanup quantum per call.
	++inFlight_;
	const bool archiveBusy = !archive_.idle();
	const bool readerWork = restoreOwner_.request && restoreOwner_.checkpoint
	                        && (restoreOwner_.canceled || (!restoreOwner_.ioReady && !restoreOwner_.readerFinished));
	bool serviceWriter = archiveBusy;
	if (archiveBusy && readerWork) {
		serviceWriter = !sdPreferRestore_;
		sdPreferRestore_ = !sdPreferRestore_;
	}
	if (serviceWriter) {
		archive_.service(maxBytes);
		--inFlight_;
		return;
	}
	if (restoreOwner_.request && restoreOwner_.checkpoint && restoreOwner_.canceled) {
		archive_.serviceCheckpointReadCleanup();
		if (archive_.checkpointReadIdle()) {
			if (restoreOwner_.candidate.valid())
				pool_.retire(restoreOwner_.candidate);
			restoreOwner_ = {};
		}
		--inFlight_;
		return;
	}
	if (restoreOwner_.request && restoreOwner_.checkpoint && !restoreOwner_.ioReady && !restoreOwner_.readerFinished) {
		const auto request = restoreOwner_.request;
		const auto slot = restoreOwner_.slot;
		const auto identity = restoreOwner_.checkpointIdentity;
		const auto slotGeneration = restoreOwner_.slotGeneration;
		const auto source = restoreOwner_.source;
		const auto sourceRevision = restoreOwner_.sourceRevision;
		const bool header = restoreOwner_.validatingHeader;
		const bool close = !header && restoreOwner_.cursor == restoreOwner_.totalFrames;
		if (!maxBytes || ((header || close) && maxBytes < 44) || (!header && !close && maxBytes < 6)) {
			--inFlight_;
			return;
		}
		size_t framesRead = 0;
		FrameIndex totalFrames = 0;
		Status status;
		if (header || close)
			status = archive_.readCheckpoint(slot, identity, close ? restoreOwner_.totalFrames : 0, {}, framesRead,
			                                 totalFrames);
		else {
			const auto count = std::min<size_t>(
			    {512, restoreFrames_.size(), maxBytes / 6, restoreOwner_.totalFrames - restoreOwner_.cursor});
			status = archive_.readCheckpoint(slot, identity, restoreOwner_.cursor,
			                                 std::span(restoreFrames_).first(count), framesRead, totalFrames);
		}
		if (restoreOwner_.request == request && restoreOwner_.checkpoint && !restoreOwner_.canceled && ready()
		    && restoreOwner_.modelGeneration == modelEpoch_ && restoreOwner_.slotGeneration == slotGeneration
		    && slotGenerations_[slot] == slotGeneration && restoreOwner_.source.slot == source.slot
		    && restoreOwner_.source.generation == source.generation && pool_.sealedHandle(slot).slot == source.slot
		    && pool_.sealedHandle(slot).generation == source.generation
		    && restoreOwner_.sourceRevision == sourceRevision && rollback_.readRevision(slot) == sourceRevision) {
			restoreOwner_.ioStatus = status;
			restoreOwner_.ioFramesRead = framesRead;
			restoreOwner_.ioTotalFrames = totalFrames;
			restoreOwner_.ioClose = close;
			restoreOwner_.ioReady = true;
		}
		else
			archive_.cancelCheckpointRead(slot, identity);
	}
	--inFlight_;
}
void SongModel::serviceRetirement() {
	++inFlight_;
	{
		Boundary boundary(host_);
		history_.serviceRetirement(16);
		pool_.serviceRetirement(16);
	}
	if (restoreOwner_.request && restoreOwner_.canceled && !restoreServicing_) {
		if (restoreOwner_.checkpoint) {
			if (archive_.checkpointReadIdle()) {
				if (restoreOwner_.candidate.valid()) {
					Boundary boundary(host_);
					pool_.retire(restoreOwner_.candidate);
				}
				restoreOwner_ = {};
			}
		}
		else if (!history_.restorePending())
			restoreOwner_ = {};
	}
	--inFlight_;
}
void SongModel::requestRelease() {
	Boundary boundary(host_);
	++generation_;
	if (++modelEpoch_ == 0)
		++modelEpoch_;
	releasing_ = true;
	cancelHistoryNavigation(Status::Cancelled);
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		cancelRestore(slot);
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		transpose_.release(slot);
		history_.resetSlot(slot);
		const auto live = pool_.sealedHandle(slot);
		if (live.valid())
			pool_.retire(live);
	}
	snapshots_.releaseAll();
	archive_.close();
	if (registered_) {
		for (SlotId id = 0; id < kTrackCount; ++id) {
			if (bindings_[id])
				host_.attach(bindings_[id], nullptr, kInvalidSlot);
			bindings_[id] = nullptr;
		}
		host_.registerAudio(nullptr, nullptr);
		registered_ = false;
	}
}
bool SongModel::safeToRelease() const {
	return !registered_ && inFlight_ == 0 && !restoreOwner_.request && !navigation_.active && archive_.idle()
	       && archive_.checkpointReadIdle() && !history_.restorePending() && !history_.retirementPending()
	       && !pool_.retirementPending();
}
uint16_t SongModel::maxDurationBarsForTrack(SlotId slot, uint32_t framesPerBar) {
	if (!validSlot(slot) || !ready())
		return 0;
	Boundary boundary(host_);
	return maxDurationBarsForPages(pool_.freePages(), framesPerBar);
}

Status SongModel::copySettingsTo(SlotId from, SlotId to) {
	if (!validSlot(from) || !validSlot(to))
		return Status::Invalid;
	const auto prepared = ensureBound(to);
	return prepared == Status::Ok ? copySettings(from, to) : prepared;
}

Status SongModel::copySettings(SlotId from, SlotId to) {
	if (!validSlot(from) || !validSlot(to))
		return Status::Invalid;
	if (!ready())
		return Status::Unavailable;
	const auto cents = engine_.settings().tracks[from].transposeCents;
	const auto ratio = Transpose::ratioForCentsQ24(cents);
	Boundary boundary(host_);
	// Copy directly: a general command dispatch would service pending actions.
	if (replacing(to) || engine_.state(to).capture)
		return Status::Busy;
	const auto status = engine_.copyTrackSettings(from, to);
	if (status == Status::Ok)
		transpose_.setCentsAndRatio(to, cents, ratio);
	return status;
}
Status SongModel::dispatch(Command command) {
	if (!ready())
		return Status::Unavailable;
	std::array<uint32_t, kTrackCount> configureRatios{};
	if (command.type == CommandType::Configure)
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			configureRatios[slot] = Transpose::ratioForCentsQ24(command.configuration.tracks[slot].transposeCents);
	Boundary boundary(host_);
	// Required transport teardown invalidates accepted restore/navigation work.
	if (command.type == CommandType::TransportStop || command.type == CommandType::Exit) {
		for (SlotId slot = 0; slot < kTrackCount; ++slot) {
			cancelRestore(slot);
		}
		if (navigation_.active)
			cancelHistoryNavigation(Status::Stale);
		++generation_;
	}
	if ((command.type == CommandType::Press || command.type == CommandType::Release
	     || command.type == CommandType::Select)
	    && (!validSlot(command.slot) || !bindings_[command.slot]))
		return Status::Unavailable;
	const bool hadPhrase = validSlot(command.slot) ? engine_.state(command.slot).hasPhrase : false;
	const auto status = engine_.dispatch(command, host_.now(), clock_);
	// Invalidations are decided by the effect a pad/selection event actually had:
	// a new capture, a pending initial take, or a Clear. Harmless presses, obsolete
	// releases and selection must not abort accepted work.
	if (validSlot(command.slot) && (command.type == CommandType::Press || command.type == CommandType::Release)) {
		const auto state = engine_.state(command.slot);
		const bool newCapture = state.capture || (state.pending && state.action == PendingAction::Initial);
		const bool cleared = hadPhrase && !state.hasPhrase;
		if (newCapture || cleared || replacing(command.slot)) {
			if (navigation_.active && navigation_.slot == command.slot)
				cancelHistoryNavigation(Status::Stale);
			cancelRestore(command.slot);
		}
	}
	if (status == Status::Ok && command.type == CommandType::Configure)
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			transpose_.setCentsAndRatio(slot, engine_.settings().tracks[slot].transposeCents, configureRatios[slot]);
	return status;
}
void SongModel::updateClock(SampleClock now, uint32_t bpmQ16, bool running, int64_t metronomeTickQ16) {
	if (!ready())
		return;
	Boundary boundary(host_);
	if (tempo_ != bpmQ16) {
		clock_.setTempoQ16(now, bpmQ16);
		tempo_ = bpmQ16;
	}
	if (running_ && !running) {
		++generation_;
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			cancelRestore(slot);
		cancelHistoryNavigation(Status::Cancelled);
		engine_.dispatch({CommandType::TransportStop}, now, clock_);
	}
	if (running_ != running)
		advanceTransportEpoch();
	clock_.setTransport(running, now);
	if (running)
		clock_.setMetronomePositionQ16(now, metronomeTickQ16);
	running_ = running;
}
} // namespace loop_station
