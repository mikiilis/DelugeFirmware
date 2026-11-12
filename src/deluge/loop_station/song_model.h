#pragma once
#include "archive.h"
#include "audio_adapter.h"
#include "history.h"
#include "memory_reservation.h"
#include "snapshot.h"
#include "transpose.h"
#include <optional>
class AudioClip;
namespace loop_station {
// Firmware implements these operations on the cooperative foreground/audio owner.
// prepare may yield. Publication, attachment and registration run inside Boundary
// and MUST NOT yield. A deleted pending clip is reported before its storage is freed.
class BindingHost {
public:
	virtual ~BindingHost() = default;
	virtual AudioClip* allocateClip() = 0;
	virtual Status prepareClip(AudioClip*, SlotId) = 0;
	virtual Status publishClip(AudioClip*) = 0;
	virtual void discardClip(AudioClip*) = 0;
	virtual Status attach(AudioClip*, AudioAdapter*, SlotId) = 0;
	virtual bool enterBoundary() = 0;
	virtual void leaveBoundary(bool previous) = 0;
	virtual void registerAudio(AudioAdapter*, const ClockMap*) = 0;
	virtual SampleClock now() const = 0;
};
class SongModel {
public:
	SongModel(ReservationAllocator& allocator, BindingHost& host, ArchiveStorage* storage = nullptr)
	    : host_(host), memory_(allocator) {
		archive_.setStorage(storage);
	}
	~SongModel();
	SongModel(const SongModel&) = delete;
	SongModel& operator=(const SongModel&) = delete;
	Status start();
	Status ensureBound(SlotId);
	// Existing/imported clips: the first valid slot/clip binding wins.
	Status bindExisting(SlotId, AudioClip*);
	Status unbind(SlotId);
	void clipDeleted(AudioClip*);
	void requestExit();
	Status requestTransportStop();
	Status requestViewExit();
	Status requestSongReplacement(bool discardUnsaved);
	Status requestSongReplacement(bool discardUnsaved, const std::array<Generation, kTrackCount>& confirmedRevisions,
	                              uint8_t confirmedCaptureMask);
	bool hasUnsavedAudio() const;
	void markAudioSaved(SlotId slot, Generation revision);
	uint8_t captureActivityMask() const;
	void requestRelease();
	bool safeToRelease() const;
	Status copySettings(SlotId from, SlotId to);
	Status copySettingsTo(SlotId from, SlotId to);
	Status dispatch(Command command);
	void updateClock(SampleClock now, uint32_t bpmQ16, bool running, int64_t metronomeTickQ16);
	Archive& archive() { return archive_; }
	void serviceArchive(uint32_t maxBytes);
	Status requestArchive(SlotId slot);
	Status requestRestore(SlotId slot, bool redo, RestoreId& id);
	bool canUndo(SlotId slot) const { return ready() && history_.canUndo(slot); }
	bool canRedo(SlotId slot) const { return ready() && history_.canRedo(slot); }
	Status requestCheckpointRestore(SlotId slot, Generation checkpointIdentity, RestoreId& id);
	Status serviceRestore(RestoreId id, uint32_t maxFrames);
	Status commitRestore(RestoreId id);
	void cancelRestore(SlotId slot);
	void serviceRetirement(); // Bounded CPU-only foreground reclamation; no SD access.
	Engine& engine() { return engine_; }
	uint16_t maxDurationBarsForTrack(SlotId slot, uint32_t framesPerBar);
	AudioAdapter& adapter() { return adapter_; }
	const ClockMap& clock() const { return clock_; }
	AudioClip* clip(SlotId slot) const { return validSlot(slot) ? bindings_[slot] : nullptr; }
	bool provisional(SlotId slot) const {
		return validSlot(slot) && (rollback_.provisional(slot) || rollback_.pending(slot));
	}
	Generation contentRevision(SlotId slot) const { return audio_.contentRevision(slot); }
	bool checkpointRestorePending(SlotId slot) const {
		return restoreOwner_.request && restoreOwner_.checkpoint && restoreOwner_.slot == slot;
	}
	bool explicitlyUnbound(SlotId slot) const { return validSlot(slot) && explicitlyUnbound_[slot]; }
	bool ready() const { return ready_ && !releasing_; }
	bool replacing(SlotId slot) const;

private:
	class Boundary {
	public:
		explicit Boundary(BindingHost& host) : host_(host), previous_(host.enterBoundary()) {}
		~Boundary() { host_.leaveBoundary(previous_); }

	private:
		BindingHost& host_;
		bool previous_;
	};
	BindingHost& host_;
	MemoryReservation memory_;
	std::optional<MemoryReservation::Pin> pin_; // Last arena consumer to be destroyed.
	PagePool pool_;
	GestureRollback rollback_{pool_};
	History history_{pool_, rollback_};
	SnapshotStore snapshots_{pool_, rollback_};
	Archive archive_{pool_, rollback_, snapshots_};
	ResidentAudio audio_{pool_, rollback_};
	Transpose transpose_{};
	Engine engine_{audio_, rollback_};
	AudioAdapter adapter_{engine_, audio_, pool_, transpose_};
	ClockMap clock_;
	std::array<AudioClip*, kTrackCount> bindings_{}, pending_{};
	std::array<bool, kTrackCount> settingUp_{};
	std::array<bool, kTrackCount> explicitlyUnbound_{};
	std::array<Generation, kTrackCount> slotGenerations_{};
	std::array<Generation, kTrackCount> savedRevisions_{};
	std::array<bool, kTrackCount> hasSavedRevision_{};
	Generation generation_ = 0;
	struct RestoreOwner {
		SlotId slot = kInvalidSlot;
		Generation request = 0, slotGeneration = 0, checkpointIdentity = 0, modelGeneration = 0, sourceRevision = 0;
		RestoreId historyId{};
		PhraseHandle candidate{}, source{};
		FrameIndex cursor = 0, totalFrames = 0, ioTotalFrames = 0;
		size_t ioFramesRead = 0;
		size_t ioFrameOffset = 0;
		Status ioStatus = Status::Busy;
		bool checkpoint = false, validatingHeader = false, readerFinished = false, canceled = false, ioReady = false,
		     ioClose = false;
	} restoreOwner_{};
	std::array<Frame, 512> restoreFrames_{};
	Generation restoreSequence_ = 0;
	bool restoreServicing_ = false;
	bool restoreIoServicing_ = false;
	bool sdPreferRestore_ = false;
	unsigned inFlight_ = 0;
	bool ready_ = false, registered_ = false, releasing_ = false, running_ = false;
	uint32_t tempo_ = 0;
};
} // namespace loop_station
