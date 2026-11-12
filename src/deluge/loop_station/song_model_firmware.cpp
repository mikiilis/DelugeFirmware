#include "RZA1/cpu_specific.h"
#include "archive_storage.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/audio_clip.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/audio_output.h"
#include "processing/engines/audio_engine.h"
#include "song_model.h"
#include "storage/storage_manager.h"
#include <algorithm>
#include <new>

namespace loop_station {
// Heap allocated in external RAM: fixed metadata and the 9,276-byte adapter never
// occupy an audio callback stack. No allocation happens until explicit enablement.
struct FirmwareOwner final : ReservationAllocator, BindingHost {
	explicit FirmwareOwner(Song& song) : song(song), model(*this, *this, &storage) {}
	Song& song;
	FatFsArchiveStorage storage{&fileSystem};
	SongModel model; // Dies before its allocator and host.
	std::array<SavedPhrase, kTrackCount> restorePhrases{};
	uint8_t restoreMask = 0;
	RestoreId activeRestore{};
	SlotId activeRestoreSlot = kInvalidSlot;

	Status rebuildPublishedPaths(const Manifest& manifest) {
		storage.clearPublishedFiles();
		for (unsigned i = 0; i < manifest.phraseCount; ++i) {
			const auto& phrase = manifest.phrases[i];
			const auto status = storage.registerPublishedFile(phrase.slot, phrase.identity, phrase.path.data());
			if (status != Status::Ok)
				return status;
		}
		return Status::Ok;
	}

	void beginCheckpointRestores(const PersistenceState& persistence) {
		restoreMask = 0;
		for (SlotId slot = 0; slot < kTrackCount; ++slot) {
			if (!persistence.loadAdmission().admitted[slot])
				continue;
			restorePhrases[slot] = persistence.loadAdmission().phrases[slot];
			restoreMask |= uint8_t(1u << slot);
		}
		startNextRestore();
	}

	void startNextRestore() {
		if (activeRestore.valid())
			return;
		for (SlotId slot = 0; slot < kTrackCount; ++slot) {
			const auto bit = uint8_t(1u << slot);
			if (!(restoreMask & bit))
				continue;
			auto status = model.ensureBound(slot);
			if (status == Status::Ok)
				status = model.requestCheckpointRestore(slot, restorePhrases[slot].identity, activeRestore);
			if (status == Status::Ok) {
				activeRestoreSlot = slot;
				return;
			}
			restoreMask &= static_cast<uint8_t>(~bit);
		}
	}

	void serviceCheckpointRestore() {
		if (activeRestore.valid()) {
			const auto status = model.serviceRestore(activeRestore, 128);
			if (status == Status::Ok) {
				const auto committed = model.commitRestore(activeRestore);
				if (committed == Status::Busy)
					return;
			}
			else if (status == Status::Busy)
				return;
			else
				model.cancelRestore(activeRestoreSlot);
			restoreMask &= static_cast<uint8_t>(~(1u << activeRestoreSlot));
			activeRestore = {};
			activeRestoreSlot = kInvalidSlot;
		}
		startNextRestore();
	}
	void* allocatePinnedLowSpeed(size_t bytes) override {
		auto& allocator = GeneralMemoryAllocator::get();
		auto* address = allocator.allocLowSpeed(bytes);
		if (!address)
			return nullptr;
		const auto first = reinterpret_cast<uintptr_t>(address);
		const auto header = *(reinterpret_cast<const uint32_t*>(address) - 1);
		if (first < EXTERNAL_MEMORY_BEGIN || first >= EXTERNAL_MEMORY_END || bytes > EXTERNAL_MEMORY_END - first
		    || (header & SPACE_TYPE_MASK) != SPACE_HEADER_ALLOCATED) {
			allocator.dealloc(address);
			return nullptr;
		}
		return address;
	}
	void deallocate(void* address) override { GeneralMemoryAllocator::get().dealloc(address); }
	AudioClip* allocateClip() override {
		auto* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(AudioClip));
		if (!memory)
			return nullptr;
		auto* clip = new (memory) AudioClip;
		clip->setResidentOwner(&model);
		return clip;
	}
	Status prepareClip(AudioClip* clip, SlotId slot) override {
		auto* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(AudioOutput));
		if (!memory)
			return Status::NoMemory;
		auto* output = new (memory) AudioOutput;
		clip->output = output; // Unpublished: neither song render nor session sees it.
		char name[] = "LOOP 1";
		name[5] += slot;
		if (output->name.set(name) != Error::NONE || clip->name.set(name) != Error::NONE
		    || clip->paramManager.setupUnpatched() != Error::NONE)
			return Status::NoMemory;
		GlobalEffectableForClip::initParamsForAudioClip(&clip->paramManager);
		clip->loopLength = song.getBarLength();
		clip->originalLength = clip->loopLength;
		clip->armedForRecording = false;
		clip->activeIfNoSolo = false;
		// Reserve publication capacity while still outside the audio boundary.
		if (!song.sessionClips.ensureEnoughSpaceAllocated(kTrackCount))
			return Status::NoMemory;
		return Status::Ok;
	}
	Status publishClip(AudioClip* clip) override {
		if (song.sessionClips.insertClipAtIndex(clip, song.sessionClips.getNumElements()) != Error::NONE)
			return Status::NoMemory;
		song.addOutput(clip->output);
		char stack[MODEL_STACK_MAX_SIZE];
		clip->output->setActiveClip(setupModelStackWithTimelineCounter(stack, &song, clip));
		return Status::Ok;
	}
	void discardClip(AudioClip* clip) override {
		auto* output = static_cast<AudioOutput*>(clip->output);
		clip->setResidentOwner(nullptr);
		clip->output = nullptr;
		clip->~AudioClip();
		deallocate(clip);
		if (output) {
			output->~AudioOutput();
			deallocate(output);
		}
	}
	Status attach(AudioClip* clip, AudioAdapter* adapter, SlotId slot) override {
		// Imported bindings must remain independent: never steal another track's
		// output or silently suppress an ordinary active clip.
		if (adapter) {
			if (!clip->output || clip->output->type != OutputType::AUDIO)
				return Status::Invalid;
			if (clip->hasResidentAudio())
				return Status::Busy;
			for (auto* list : {&song.sessionClips, &song.arrangementOnlyClips}) {
				for (int i = 0; i < list->getNumElements(); ++i) {
					auto* other = list->getClipAtIndex(i);
					if (other != clip && other->output == clip->output)
						return Status::Busy;
				}
			}
		}
		const auto status = clip->bindResidentAudio(adapter, slot);
		if (status == Status::Ok) {
			clip->setResidentOwner(adapter ? &model : nullptr);
			if (adapter) {
				char stack[MODEL_STACK_MAX_SIZE];
				clip->output->setActiveClip(setupModelStackWithTimelineCounter(stack, &song, clip));
			}
		}
		return status;
	}
	bool enterBoundary() override {
		const bool old = AudioEngine::audioRoutineLocked;
		AudioEngine::audioRoutineLocked = true;
		return old;
	}
	void leaveBoundary(bool previous) override { AudioEngine::audioRoutineLocked = previous; }
	void registerAudio(AudioAdapter* adapter, const ClockMap* clock) override {
		AudioEngine::setLoopStationAudioAdapter(adapter, clock);
	}
	SampleClock now() const override { return AudioEngine::loopStationSampleClock(); }
};
} // namespace loop_station

loop_station::SongModel* Song::loopStation() const {
	return loopStationOwner_ ? &loopStationOwner_->model : nullptr;
}
loop_station::Status Song::beginLoopStationSave() {
	return loopStationPersistence().requestSave(loopStation());
}
loop_station::Status Song::commitLoopStationSave() {
	std::array<loop_station::Generation, loop_station::kTrackCount> revisions{};
	if (loopStationOwner_)
		for (loop_station::SlotId slot = 0; slot < loop_station::kTrackCount; ++slot)
			revisions[slot] = loopStationOwner_->model.contentRevision(slot);
	const auto status = loopStationPersistence().commitPendingAfterDocumentVerified(revisions);
	if (status == loop_station::Status::Ok && loopStationOwner_) {
		for (loop_station::SlotId slot = 0; slot < loop_station::kTrackCount; ++slot)
			if (loopStationPersistence().hasCommittedAudioRevision(slot))
				loopStationOwner_->model.markAudioSaved(slot, loopStationPersistence().committedAudioRevision(slot));
		loopStationOwner_->rebuildPublishedPaths(loopStationPersistence().committedManifest());
	}
	return status;
}
void Song::cancelLoopStationSave() {
	loopStationPersistence().cancelPending(loopStation());
	if (loopStationOwner_)
		loopStationOwner_->rebuildPublishedPaths(loopStationPersistence().committedManifest());
}
void Song::failLoopStationSave() {
	loopStationPersistence().failPending(loopStation());
	if (loopStationOwner_)
		loopStationOwner_->rebuildPublishedPaths(loopStationPersistence().committedManifest());
}
void Song::serviceLoopStationArchive(uint32_t maxBytes) {
	if (!loopStationOwner_)
		return;
	loopStationOwner_->model.serviceArchive(maxBytes);
	loopStationPersistence().serviceArchiveCandidates(loopStationOwner_->model);
	loopStationOwner_->serviceCheckpointRestore();
}
loop_station::Status Song::ensureLoopStation(bool enabled) {
	using namespace loop_station;
	if (!enabled || this != currentSong)
		return Status::Unavailable;
	if (loopStationOwner_)
		return loopStationOwner_->model.ready() ? Status::Ok : Status::Busy;
	auto* memory = GeneralMemoryAllocator::get().allocExternal(sizeof(FirmwareOwner));
	if (!memory)
		return Status::NoMemory;
	loopStationOwner_ = new (memory) FirmwareOwner(*this);
	auto status = loopStationOwner_->model.start();
	if (status == Status::Ok)
		status = loopStationOwner_->model.dispatch(Command::configure(loopStationPersistence().liveSettings()));
	if (status == Status::Ok)
		status = loopStationOwner_->rebuildPublishedPaths(loopStationPersistence().committedManifest());
	if (status == Status::Ok)
		loopStationOwner_->beginCheckpointRestores(loopStationPersistence());
	if (status != Status::Ok)
		releaseLoopStation();
	else
		updateLoopStationClock();
	return status;
}
void Song::detachLoopStation() {
	if (loopStationOwner_)
		loopStationOwner_->model.requestRelease();
}
bool Song::releaseLoopStation() {
	if (!loopStationOwner_)
		return true;
	detachLoopStation();
	if (!loopStationOwner_->model.safeToRelease())
		return false;
	auto* owner = loopStationOwner_;
	loopStationOwner_ = nullptr;
	owner->~FirmwareOwner();
	GeneralMemoryAllocator::get().dealloc(owner);
	return true;
}
void Song::updateLoopStationClock() {
	if (!loopStationOwner_ || this != currentSong)
		return;
	const bool running = playbackHandler.isEitherClockActive();
	const float bpm = running ? calculateBPM(playbackHandler.getTimePerInternalTickFloat()) : calculateBPM();
	const auto bpmQ16 = uint32_t(std::clamp(double(bpm) * 65536.0, 1.0, 1000.0 * 65536.0) + 0.5);
	loopStationOwner_->model.updateClock(AudioEngine::loopStationSampleClock(), bpmQ16, running,
	                                     running ? playbackHandler.getLoopStationMetronomeTickQ16() : 0);
}
