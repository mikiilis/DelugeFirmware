#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/archive_storage.h"
#include "deluge/loop_station/persistence.h"
#include "deluge/loop_station/song_model.h"
#include "mocks/audio_clip.h"
#include "mocks/storage.h"
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
using namespace loop_station;
namespace {
struct Allocator : ReservationAllocator {
	unsigned calls = 0, fail = 0, live = 0;
	std::function<void()> beforeFree;
	void* allocatePinnedLowSpeed(size_t n) override {
		if (++calls == fail)
			return nullptr;
		++live;
		return std::malloc(n);
	}
	void deallocate(void* p) override {
		if (beforeFree)
			beforeFree();
		--live;
		std::free(p);
	}
};
struct Host : BindingHost {
	std::vector<AudioClip*> published;
	std::function<void(AudioClip*)> yield;
	AudioAdapter* registered = nullptr;
	SampleClock time = 100;
	unsigned step = 0, fail = 0, outputs = 0, allocated = 0;
	bool locked = false;
	bool succeeds() { return ++step != fail; }
	AudioClip* allocateClip() override {
		if (!succeeds())
			return nullptr;
		++allocated;
		return new AudioClip;
	}
	Status prepareClip(AudioClip* c, SlotId id) override {
		if (!succeeds())
			return Status::NoMemory;
		c->output = ++outputs;
		if (!succeeds())
			return Status::NoMemory;
		c->name = "LOOP " + std::to_string(id + 1);
		// Firmware preparation also allocates clip name, parameters and list capacity.
		for (unsigned stage = 0; stage < 3; ++stage)
			if (!succeeds())
				return Status::NoMemory;
		if (yield)
			yield(c); // No dereference after a potentially deleting yield.
		return Status::Ok;
	}
	Status publishClip(AudioClip* c) override {
		CHECK(locked);
		if (!succeeds())
			return Status::NoMemory;
		published.push_back(c);
		return Status::Ok;
	}
	void discardClip(AudioClip* c) override {
		CHECK(!c->adapter);
		--allocated;
		delete c;
	}
	Status attach(AudioClip* c, AudioAdapter* a, SlotId) override {
		CHECK(locked);
		c->adapter = a;
		return Status::Ok;
	}
	bool enterBoundary() override {
		bool old = locked;
		locked = true;
		return old;
	}
	void leaveBoundary(bool old) override { locked = old; }
	void registerAudio(AudioAdapter* a, const ClockMap*) override {
		CHECK(locked);
		registered = a;
	}
	SampleClock now() const override { return time; }
	~Host() override {
		for (auto* c : published)
			discardClip(c);
		CHECK_EQUAL(0, allocated);
	}
};
struct Fixture {
	Allocator alloc;
	Host host;
	std::unique_ptr<SongModel> model = std::make_unique<SongModel>(alloc, host);
	Fixture() {
		alloc.beforeFree = [&] {
			CHECK(!host.registered);
			for (auto* c : host.published)
				CHECK(!c->adapter);
		};
	}
	~Fixture() {
		if (model) {
			model->requestRelease();
			for (unsigned i = 0; i < 10000 && !model->safeToRelease(); ++i) {
				model->serviceRetirement();
				model->serviceArchive(4096);
			}
		}
	}
	void start() {
		CHECK(Status::Ok == model->start());
		auto s = defaultSettings();
		s.startAction.mode = s.stopAction.mode = ActionMode::Immediate;
		CHECK(Status::Ok == model->dispatch(Command::configure(s)));
	}
	void render(size_t count = 8) {
		std::array<Frame, 128> input{}, output{};
		input.fill({100, -100});
		model->adapter().finalizeWindow(host.time, count, model->clock());
		for (SlotId id = 0; id < kTrackCount; ++id)
			if (model->clip(id))
				model->adapter().renderDry(id, std::span(input).first(count), std::span(output).first(count));
		host.time += count;
	}
	void record(SlotId id) {
		model->serviceRetirement();
		CHECK(Status::Ok == model->ensureBound(id));
		CHECK(Status::Ok == model->dispatch({CommandType::Press, id}));
		render();
		CHECK(Status::Ok == model->dispatch({CommandType::Press, id}));
	}
	Status driveRestore(RestoreId id, uint32_t maxFrames) {
		for (unsigned i = 0; i < 1000; ++i) {
			model->serviceArchive(4096);
			const auto status = model->serviceRestore(id, maxFrames);
			if (status != Status::Busy)
				return status;
		}
		return Status::Busy;
	}
	void drainStorage() {
		for (unsigned i = 0; i < 100 && !model->archive().checkpointReadIdle(); ++i)
			model->serviceArchive(4096);
		for (unsigned i = 0; i < 100 && model->archive().checkpointReadIdle(); ++i)
			model->serviceRetirement();
	}
};
} // namespace
TEST_GROUP(LoopBindings){};
TEST(LoopBindings, lazyIndependentOutputsAndClearKeepsIdentityAndFx) {
	Fixture f;
	f.start();
	CHECK_EQUAL(0, f.host.published.size());
	for (SlotId i = 0; i < 8; ++i) {
		f.record(i);
		auto* c = f.model->clip(i);
		STRCMP_EQUAL(("LOOP " + std::to_string(i + 1)).c_str(), c->name.c_str());
		CHECK_EQUAL(i + 1, c->output);
		CHECK(Status::Ok == f.model->ensureBound(i));
	}
	f.model->dispatch({CommandType::TransportStop});
	for (SlotId i = 0; i < 8; ++i)
		f.model->dispatch({CommandType::Press, i});
	f.host.time += 2 * kSampleRate;
	for (SlotId i = 0; i < 8; ++i) {
		auto* c = f.model->clip(i);
		f.model->dispatch({CommandType::Release, i});
		CHECK(!f.model->engine().state(i).hasPhrase);
		POINTERS_EQUAL(c, f.model->clip(i));
		CHECK_EQUAL(99, c->fx);
	}
	CHECK_EQUAL(8, f.host.published.size());
}
TEST(LoopBindings, exitKeepsPlayingAndDuplicateBindingsKeepFirst) {
	Fixture f;
	f.start();
	f.record(0);
	f.record(1);
	auto* first = f.model->clip(0);
	CHECK(Status::Busy == f.model->bindExisting(0, f.model->clip(1)));
	CHECK(Status::Busy == f.model->bindExisting(2, first));
	f.model->requestExit();
	f.render(3);
	CHECK(f.model->engine().state(0).playing);
	CHECK_EQUAL(3, f.model->adapter().phase(0));
	POINTERS_EQUAL(first, f.model->clip(0));
	CHECK(f.host.registered);
}
TEST(LoopBindings, yieldingExitReopenAndDeleteCannotPublishLateSetup) {
	Fixture f;
	f.start();
	f.host.yield = [&](AudioClip*) {
		f.model->requestExit();
		CHECK(Status::Busy == f.model->ensureBound(0));
	};
	CHECK(Status::Stale == f.model->ensureBound(0));
	CHECK_EQUAL(0, f.host.allocated);
	f.host.yield = [&](AudioClip* c) {
		f.model->clipDeleted(c);
		f.host.discardClip(c);
	};
	CHECK(Status::Stale == f.model->ensureBound(0));
	CHECK_EQUAL(0, f.host.allocated);
	f.host.yield = {};
	CHECK(Status::Ok == f.model->ensureBound(0));
}
TEST(LoopBindings, transportDuringSetupInvalidatesTransactionAndReleaseWaitsForIt) {
	Fixture f;
	f.start();
	f.host.yield = [&](AudioClip*) { f.model->dispatch({CommandType::TransportStop}); };
	CHECK(Status::Stale == f.model->ensureBound(0));
	f.host.yield = [&](AudioClip*) {
		f.model->requestRelease();
		CHECK(!f.model->safeToRelease());
		CHECK_EQUAL(5, f.alloc.live);
	};
	CHECK(Status::Stale == f.model->ensureBound(0));
	CHECK(f.model->safeToRelease());
	CHECK(!f.host.registered);
	f.model.reset();
	CHECK_EQUAL(0, f.alloc.live);
}
TEST(LoopBindings, everyAllocationFailureRollsBackWithoutPublishedOrBorrowedObjects) {
	for (unsigned i = 1; i <= 5; ++i) {
		Fixture f;
		f.alloc.fail = i;
		CHECK(Status::NoMemory == f.model->start());
		CHECK_EQUAL(0, f.alloc.live);
	}
	for (unsigned i = 1; i <= 7; ++i) {
		Fixture f;
		f.start();
		f.host.fail = i;
		CHECK(Status::NoMemory == f.model->ensureBound(0));
		CHECK_EQUAL(0, f.host.allocated);
		CHECK_EQUAL(0, f.host.published.size());
		CHECK(!f.model->clip(0));
	}
}
TEST(LoopBindings, durationMenuLimitTracksOnlyCurrentlyFreeAdmittedPages) {
	Fixture f;
	f.start();
	CHECK_EQUAL(1024, f.model->maxDurationBarsForTrack(0, 512));
	f.record(0);
	CHECK_EQUAL(1023, f.model->maxDurationBarsForTrack(1, 512));
	CHECK_EQUAL(0, f.model->maxDurationBarsForTrack(kInvalidSlot, 512));
}
TEST(LoopBindings, failedUnboundCopyDestinationAllocationLeavesSourceAndDestinationUntouched) {
	Fixture f;
	f.start();
	f.record(0);
	auto settings = f.model->engine().settings();
	settings.tracks[0] = {Source::Right, Monitoring::Off, 3, PlaybackMode::OneShot, Colour::Pink, 234};
	settings.tracks[1] = {Source::Stereo, Monitoring::On, 7, PlaybackMode::Loop, Colour::Cyan, -123};
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	auto* source = f.model->clip(0);
	CHECK(source);
	CHECK(!f.model->clip(1));
	const auto sourceState = f.model->engine().state(0);
	const auto destinationState = f.model->engine().state(1);
	f.host.fail = f.host.step + 1;
	CHECK(Status::NoMemory == f.model->copySettingsTo(0, 1));
	CHECK(f.model->engine().settings().tracks[0].source == Source::Right);
	CHECK(f.model->engine().settings().tracks[0].colour == Colour::Pink);
	CHECK(f.model->engine().settings().tracks[1].source == Source::Stereo);
	CHECK(f.model->engine().settings().tracks[1].colour == Colour::Cyan);
	POINTERS_EQUAL(source, f.model->clip(0));
	CHECK(!f.model->clip(1));
	CHECK_EQUAL(sourceState.phraseFrames, f.model->engine().state(0).phraseFrames);
	CHECK_EQUAL(destinationState.phraseFrames, f.model->engine().state(1).phraseFrames);
}
TEST(LoopBindings, copyOnlyConfigurationAndRejectCaptureBeforeMutation) {
	Fixture f;
	f.start();
	f.record(0);
	f.record(1);
	auto s = f.model->engine().settings();
	s.tracks[0] = {Source::Right, Monitoring::Off, 2, PlaybackMode::OneShot, Colour::Pink, 234};
	f.model->dispatch(Command::configure(s));
	f.model->dispatch({CommandType::Press, 1});
	CHECK(Status::Busy == f.model->copySettings(0, 1));
	CHECK(f.model->engine().settings().tracks[1].source == Source::Left);
	f.model->dispatch({CommandType::TransportStop});
	auto* c = f.model->clip(1);
	auto state = f.model->engine().state(1);
	CHECK(Status::Ok == f.model->copySettings(0, 1));
	auto target = f.model->engine().settings().tracks[1];
	CHECK(target.source == Source::Right);
	CHECK(target.monitoring == Monitoring::Off);
	CHECK_EQUAL(2, target.recordDurationBars);
	CHECK(target.playback == PlaybackMode::OneShot);
	CHECK(target.colour == Colour::Pink);
	CHECK_EQUAL(234, target.transposeCents);
	POINTERS_EQUAL(c, f.model->clip(1));
	CHECK_EQUAL(99, c->fx);
	CHECK_EQUAL(state.phraseFrames, f.model->engine().state(1).phraseFrames);
	CHECK(!f.model->engine().state(1).playing);
}
TEST(LoopBindings, slotLocalCaptureErrorsAndEightSlotUnbindRebindChurn) {
	Fixture f;
	f.start();
	for (SlotId i = 0; i < 8; ++i)
		f.record(i);
	f.model->dispatch({CommandType::CaptureError, 0, Status::SourceUnavailable});
	CHECK(f.model->engine().state(0).error == Status::SourceUnavailable);
	for (SlotId i = 1; i < 8; ++i)
		CHECK(f.model->engine().state(i).playing);
	for (unsigned round = 0; round < 10; ++round) {
		for (SlotId i = 0; i < 8; ++i) {
			CHECK(Status::Ok == f.model->unbind(i));
			CHECK(!f.model->clip(i));
		}
		for (SlotId i = 0; i < 8; ++i)
			f.record(i);
	}
}
TEST(LoopBindings, clockTracksTempoAndMetronomeEvenWithoutViewAndPreservesPhrasePhase) {
	Fixture f;
	f.start();
	f.record(0);
	f.render(3);
	const auto phase = f.model->adapter().phase(0);
	f.model->updateClock(f.host.time, 90 * 65536, true, 6 * 65536);
	CHECK_EQUAL(phase, f.model->adapter().phase(0));
	const auto deadline = nextDeadline({}, f.host.time, f.model->clock()).sampleAt(f.model->clock());
	CHECK_EQUAL(f.host.time + 7350, deadline);
	f.model->requestExit();
	f.model->updateClock(f.host.time, 180 * 65536, true, 6 * 65536);
	CHECK_EQUAL(phase, f.model->adapter().phase(0));
	CHECK_EQUAL(f.host.time + 3675, nextDeadline({}, f.host.time, f.model->clock()).sampleAt(f.model->clock()));
	f.model->updateClock(f.host.time, 180 * 65536, false, 0);
	CHECK(!f.model->engine().state(0).playing);
}
TEST(LoopBindings, unbindDuringProvisionalCaptureDefersRebindUntilRollbackReadersDrain) {
	Fixture f;
	f.start();
	f.record(0);
	// Make a longer phrase so this gesture retains more than one service budget.
	f.model->dispatch({CommandType::TransportStop});
	f.model->unbind(0);
	f.model->ensureBound(0);
	f.model->dispatch({CommandType::Press, 0});
	f.render(128);
	f.model->dispatch({CommandType::Press, 0});
	f.model->dispatch({CommandType::Press, 0});
	f.render(128);
	auto* c = f.model->clip(0);
	CHECK(Status::Ok == f.model->unbind(0));
	CHECK(!c->adapter);
	CHECK(Status::Busy == f.model->ensureBound(0));
	CHECK(Status::Busy == f.model->copySettings(1, 0));
	f.render();
	f.render(); // Deferred clear is strictly after the detach sample.
	CHECK(Status::Ok == f.model->ensureBound(0));
	CHECK(!f.model->engine().state(0).hasPhrase);
}
TEST(LoopBindings, deletionDetachesBeforeClipStorageIsReused) {
	Fixture f;
	f.start();
	f.record(0);
	f.record(1);
	auto* c = f.model->clip(0);
	f.model->clipDeleted(c);
	CHECK(!c->adapter);
	CHECK(!f.model->clip(0));
	CHECK(!f.model->engine().state(0).hasPhrase);
	f.render();
	CHECK(f.model->engine().state(1).playing);
	CHECK(Status::Ok == f.model->bindExisting(0, c));
}
TEST(LoopBindings, copyDoesNotDispatchDueActionsOrMutateDestinationLiveState) {
	Fixture f;
	f.start();
	f.model->ensureBound(0);
	f.model->ensureBound(1);
	auto settings = f.model->engine().settings();
	settings.startAction.mode = ActionMode::Synced;
	settings.tracks[0].source = Source::Right;
	f.model->dispatch(Command::configure(settings));
	f.model->dispatch({CommandType::Press, 1});
	f.host.time += kSampleRate;
	CHECK(Status::Ok == f.model->copySettings(0, 1));
	CHECK(f.model->engine().state(1).pending);
	CHECK(!f.model->engine().state(1).capture);
}
TEST(LoopBindings, actualMetronomeOffsetControlsNextPulseWithoutResettingPhrase) {
	Fixture f;
	f.start();
	f.record(0);
	f.render(3);
	f.model->updateClock(f.host.time, 120 * 65536, true, 3 * 65536);
	CHECK_EQUAL(f.host.time + 2757, nextDeadline({}, f.host.time, f.model->clock()).sampleAt(f.model->clock()));
	CHECK_EQUAL(3, f.model->adapter().phase(0));
}
TEST(LoopBindings, deletingOtherSlotDoesNotCancelIndependentSetup) {
	Fixture f;
	f.start();
	f.record(1);
	f.host.yield = [&](AudioClip*) { f.model->clipDeleted(f.model->clip(1)); };
	CHECK(Status::Ok == f.model->ensureBound(0));
	CHECK(f.model->clip(0));
	CHECK(!f.model->clip(1));
}
TEST(LoopBindings, pendingCandidateCannotBeBoundIntoAnotherSlot) {
	Fixture f;
	f.start();
	Status result = Status::Invalid;
	f.host.yield = [&](AudioClip* candidate) {
		result = f.model->bindExisting(1, candidate);
		if (result == Status::Ok)
			f.model->unbind(1);
	};
	CHECK(Status::Ok == f.model->ensureBound(0));
	CHECK(result == Status::Busy);
	CHECK(!f.model->clip(1));
}
TEST(LoopBindings, archiveLeasePreventsReleaseDuringStorageYieldAndRejectsReentry) {
	struct Storage : ArchiveStorage {
		std::function<void()> yielding;
		unsigned writes = 0, aborts = 0;
		Status begin(SlotId, Generation) override { return Status::Ok; }
		Status write(std::span<const std::byte> bytes, size_t& written) override {
			++writes;
			written = bytes.size();
			if (yielding) {
				auto fn = std::exchange(yielding, {});
				fn();
			}
			return Status::Ok;
		}
		Status finalize() override { return Status::Ok; }
		void abort() override { ++aborts; }
		void discard(SlotId, Generation) override {}
	} storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->requestArchive(0));
	bool safeDuring = true, yielded = false;
	storage.yielding = [&] {
		yielded = true;
		f.model->requestRelease();
		safeDuring = f.model->safeToRelease();
		f.model->serviceArchive(4096); // nested foreground tick cannot touch storage.
	};
	for (unsigned i = 0; i < 5 && !yielded; ++i)
		f.model->serviceArchive(4096);
	const bool safeBetweenSteps = f.model->safeToRelease();
	for (unsigned i = 0; i < 5; ++i)
		f.model->serviceArchive(4096), f.model->serviceRetirement();
	CHECK_FALSE(safeDuring);
	CHECK_FALSE(safeBetweenSteps);
	CHECK(f.model->safeToRelease());
	CHECK_EQUAL(1, storage.writes);
	CHECK_EQUAL(1, storage.aborts);
	CHECK(Status::Unavailable == f.model->requestArchive(0));
}
TEST(LoopBindings, unbindImmediatelyCancelsReadyArchiveAndPreventsNewAdmission) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->requestArchive(0));
	for (unsigned i = 0; i < 12; ++i)
		f.model->serviceArchive(4096);
	CHECK(ArchiveState::ReadyForPublication == f.model->archive().state(0));
	CHECK(Status::Ok == f.model->unbind(0));
	CHECK(ArchiveState::Cancelled == f.model->archive().state(0));
	CHECK(Status::Unavailable == f.model->requestArchive(0));
	f.model->serviceArchive(4096);
	CHECK_EQUAL(0, archive_mock::storage.files.size());
}
TEST(LoopBindings, releaseWaitsForResumableActiveCloseAndUnlinkBetweenServices) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->requestArchive(0));
	for (unsigned i = 0; i < 5 && archive_mock::storage.open.empty(); ++i)
		f.model->serviceArchive(4096);
	CHECK_EQUAL(1, archive_mock::storage.open.size());
	f.model->requestRelease();
	const bool beforeCleanup = f.model->safeToRelease();
	f.model->serviceArchive(4096); // Record abort request, without a storage call.
	f.model->serviceRetirement();
	f.model->serviceArchive(4096); // Close only; exclusive unlink is still pending.
	f.model->serviceRetirement();
	const bool afterClose = f.model->safeToRelease();
	const auto closes = archive_mock::storage.calls["close"], unlinksBefore = archive_mock::storage.calls["unlink"];
	f.model->serviceArchive(4096); // Unlink and release the persistent active-job lease.
	f.model->serviceRetirement();
	for (unsigned i = 0; i < 100 && !f.model->safeToRelease(); ++i)
		f.model->serviceRetirement();
	CHECK_FALSE(beforeCleanup);
	CHECK_FALSE(afterClose);
	CHECK(f.model->safeToRelease());
	CHECK_EQUAL(1, closes);
	CHECK_EQUAL(0, unlinksBefore);
	CHECK_EQUAL(1, archive_mock::storage.calls["unlink"]);
}
TEST(LoopBindings, releaseAbandonsOnlyInactiveReadyCandidatesWithoutCardIo) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->requestArchive(0));
	for (unsigned i = 0; i < 15; ++i)
		f.model->serviceArchive(4096);
	CHECK(ArchiveState::ReadyForPublication == f.model->archive().state(0));
	const auto calls = archive_mock::storage.operations.size();
	f.model->requestRelease();
	for (unsigned i = 0; i < 100 && !f.model->safeToRelease(); ++i)
		f.model->serviceRetirement(), f.model->serviceArchive(4096);
	CHECK(f.model->safeToRelease());
	CHECK_EQUAL(calls, archive_mock::storage.operations.size());
	CHECK_EQUAL(1, archive_mock::storage.files.size()); // Unreferenced orphan belongs to manifest recovery.
}

TEST(LoopBindings, checkpointRestoreStagesOffPathAndCommitsEngineMetadataAtBoundary) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->requestArchive(0));
	for (unsigned i = 0; i < 20 && f.model->archive().state(0) != ArchiveState::ReadyForPublication; ++i)
		f.model->serviceArchive(4096);
	ArchiveCandidate checkpoint{};
	CHECK(f.model->archive().take(0, checkpoint));
	const auto before = f.model->engine().state(0);
	RestoreId restore{};
	CHECK(Status::Ok == f.model->requestCheckpointRestore(0, checkpoint.identity, restore));
	CHECK(Status::Busy == f.model->serviceRestore(restore, 2));
	CHECK_EQUAL(before.phraseFrames, f.model->engine().state(0).phraseFrames);
	CHECK_EQUAL(before.playing, f.model->engine().state(0).playing);
	CHECK(Status::Ok == f.driveRestore(restore, 512));
	CHECK_EQUAL(before.phraseFrames, f.model->engine().state(0).phraseFrames);
	CHECK(Status::Ok == f.model->commitRestore(restore));
	CHECK_EQUAL(before.phraseFrames, f.model->engine().state(0).phraseFrames);
	CHECK_EQUAL(before.playing, f.model->engine().state(0).playing);
	CHECK(f.model->engine().state(0).hasPhrase);
}

TEST(LoopBindings, checkpointLoadCanRestoreIntoAnEmptyBoundLoopSlot) {
	archive_mock::storage = {};
	ArchiveCandidate checkpoint{};
	{
		FatFsArchiveStorage writerStorage;
		Fixture source;
		source.model->archive().setStorage(&writerStorage);
		source.start();
		source.record(0);
		CHECK(Status::Ok == source.model->requestArchive(0));
		for (unsigned i = 0; i < 20 && source.model->archive().state(0) != ArchiveState::ReadyForPublication; ++i)
			source.model->serviceArchive(4096);
		CHECK(source.model->archive().take(0, checkpoint));
	}

	FatFsArchiveStorage readerStorage;
	Fixture loaded;
	loaded.model->archive().setStorage(&readerStorage);
	loaded.start();
	CHECK(Status::Ok
	      == readerStorage.registerPublishedFile(checkpoint.slot, checkpoint.identity, checkpoint.path.data()));
	CHECK(Status::Ok == loaded.model->ensureBound(checkpoint.slot));
	CHECK_FALSE(loaded.model->engine().state(checkpoint.slot).hasPhrase);
	RestoreId restore{};
	CHECK(Status::Ok == loaded.model->requestCheckpointRestore(checkpoint.slot, checkpoint.identity, restore));
	CHECK(Status::Ok == loaded.driveRestore(restore, 128));
	CHECK(Status::Ok == loaded.model->commitRestore(restore));
	CHECK(loaded.model->engine().state(checkpoint.slot).hasPhrase);
	CHECK_EQUAL(checkpoint.frames, loaded.model->engine().state(checkpoint.slot).phraseFrames);
	CHECK_FALSE(loaded.model->hasUnsavedAudio());
}

TEST(LoopBindings, saveDuringCheckpointRestoreRetainsCommittedManifestReference) {
	archive_mock::storage = {};
	ArchiveCandidate checkpoint{};
	{
		FatFsArchiveStorage writerStorage;
		Fixture source;
		source.model->archive().setStorage(&writerStorage);
		source.start();
		source.record(0);
		CHECK(Status::Ok == source.model->requestArchive(0));
		for (unsigned i = 0; i < 20 && source.model->archive().state(0) != ArchiveState::ReadyForPublication; ++i)
			source.model->serviceArchive(4096);
		CHECK(source.model->archive().take(0, checkpoint));
	}

	FatFsArchiveStorage readerStorage;
	Fixture loaded;
	loaded.model->archive().setStorage(&readerStorage);
	loaded.start();
	CHECK(Status::Ok
	      == readerStorage.registerPublishedFile(checkpoint.slot, checkpoint.identity, checkpoint.path.data()));
	CHECK(Status::Ok == loaded.model->ensureBound(checkpoint.slot));
	RestoreId restore{};
	CHECK(Status::Ok == loaded.model->requestCheckpointRestore(checkpoint.slot, checkpoint.identity, restore));
	CHECK(loaded.model->checkpointRestorePending(checkpoint.slot));

	Manifest committed{};
	committed.generation = 1;
	committed.phraseCount = 1;
	committed.phrases[0].slot = checkpoint.slot;
	committed.phrases[0].identity = checkpoint.identity;
	committed.phrases[0].frames = checkpoint.frames;
	std::strncpy(committed.phrases[0].path.data(), checkpoint.path.data(), committed.phrases[0].path.size() - 1);
	PersistenceState persistence;
	CHECK(Status::Ok == persistence.installLoaded(defaultSettings(), committed));
	CHECK(Status::Ok == persistence.requestSave(loaded.model.get()));
	CHECK(persistence.pendingReady());
	CHECK_EQUAL(1, persistence.pendingManifest().phraseCount);
	CHECK_EQUAL(checkpoint.identity, persistence.pendingManifest().phrases[0].identity);
}

TEST(LoopBindings, successfulArchiveSaveCommitsTheRevisionRepresentedByTheManifest) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0);
	PersistenceState persistence;
	CHECK(Status::Ok == persistence.installLoaded(defaultSettings(), Manifest{}));
	CHECK(f.model->hasUnsavedAudio());
	CHECK(Status::Ok == persistence.requestSave(f.model.get()));
	for (unsigned i = 0; i < 64 && !persistence.pendingReady(); ++i) {
		f.model->serviceArchive(4096);
		persistence.serviceArchiveCandidates(*f.model);
	}
	CHECK(persistence.pendingReady());
	std::array<Generation, kTrackCount> revisions{};
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		revisions[slot] = f.model->contentRevision(slot);
	CHECK(Status::Ok == persistence.commitPendingAfterDocumentVerified(revisions));
	CHECK(persistence.hasCommittedAudioRevision(0));
	CHECK_EQUAL(revisions[0], persistence.committedAudioRevision(0));
	f.model->markAudioSaved(0, persistence.committedAudioRevision(0));
	CHECK_FALSE(f.model->hasUnsavedAudio());
}

TEST(LoopBindings, failedCheckpointReadAndCancellationLeaveLiveEngineUntouched) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->requestArchive(0));
	for (unsigned i = 0; i < 20 && f.model->archive().state(0) != ArchiveState::ReadyForPublication; ++i)
		f.model->serviceArchive(4096);
	ArchiveCandidate checkpoint{};
	CHECK(f.model->archive().take(0, checkpoint));
	const auto before = f.model->engine().state(0);
	RestoreId restore{};
	CHECK(Status::Ok == f.model->requestCheckpointRestore(0, checkpoint.identity, restore));
	CHECK(Status::Busy == f.model->serviceRestore(restore, 1));
	f.model->cancelRestore(0);
	CHECK(Status::Stale == f.model->serviceRestore(restore, 1));
	CHECK_EQUAL(before.phraseFrames, f.model->engine().state(0).phraseFrames);
	CHECK_EQUAL(before.playing, f.model->engine().state(0).playing);
	CHECK_EQUAL(before.capture, f.model->engine().state(0).capture);
	const auto& data = archive_mock::storage.files[checkpoint.path.data()];
	CHECK(data.size() > 8);
	auto& mutableData = archive_mock::storage.files[checkpoint.path.data()];
	mutableData[0] = std::byte{'X'};
	CHECK(Status::Ok == f.model->requestCheckpointRestore(0, checkpoint.identity, restore));
	CHECK(Status::StorageError == f.driveRestore(restore, 512));
	f.drainStorage();
	CHECK_EQUAL(before.phraseFrames, f.model->engine().state(0).phraseFrames);
	CHECK_EQUAL(before.playing, f.model->engine().state(0).playing);
}

TEST(LoopBindings, failingCheckpointChunkDuringPlaybackNeverExposesPartialCandidate) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->requestArchive(0));
	for (unsigned i = 0; i < 20 && f.model->archive().state(0) != ArchiveState::ReadyForPublication; ++i)
		f.model->serviceArchive(4096);
	ArchiveCandidate checkpoint{};
	CHECK(f.model->archive().take(0, checkpoint));
	const auto before = f.model->engine().state(0);
	RestoreId restore{};
	CHECK(Status::Ok == f.model->requestCheckpointRestore(0, checkpoint.identity, restore));
	archive_mock::storage.failOperation = "read";
	archive_mock::storage.failAfter = 1; // Validate the header, then fail the first data read.
	archive_mock::storage.failure = FR_DISK_ERR;
	unsigned reentrantRenders = 0;
	archive_mock::storage.yielding = [&](const char* operation) {
		if (std::string(operation) == "read") {
			++reentrantRenders;
			f.render(1); // Simulate playback continuing while foreground media work yields.
		}
	};
	const auto failed = f.driveRestore(restore, 2);
	CHECK_EQUAL(int(Status::StorageError), int(failed));
	CHECK(reentrantRenders >= 2);
	CHECK_EQUAL(before.phraseFrames, f.model->engine().state(0).phraseFrames);
	CHECK_EQUAL(before.playing, f.model->engine().state(0).playing);
	CHECK_EQUAL(before.capture, f.model->engine().state(0).capture);
	CHECK(Status::Stale == f.model->serviceRestore(restore, 2));
	f.drainStorage();
}

TEST(LoopBindings, releaseDuringYieldingCheckpointOpenClosesReaderBeforeSafeRelease) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->requestArchive(0));
	for (unsigned i = 0; i < 20 && f.model->archive().state(0) != ArchiveState::ReadyForPublication; ++i)
		f.model->serviceArchive(4096);
	ArchiveCandidate checkpoint{};
	CHECK(f.model->archive().take(0, checkpoint));
	RestoreId restore{};
	CHECK(Status::Ok == f.model->requestCheckpointRestore(0, checkpoint.identity, restore));
	bool releasedDuringOpen = false;
	archive_mock::storage.yielding = [&](const char* operation) {
		if (!releasedDuringOpen && std::string(operation) == "open") {
			releasedDuringOpen = true;
			f.model->requestRelease(); // Reentrant owner release while f_open_archive yields.
		}
	};
	CHECK(Status::Stale == f.driveRestore(restore, 32));
	CHECK(releasedDuringOpen);
	for (unsigned i = 0; i < 1000 && !f.model->safeToRelease(); ++i) {
		f.model->serviceArchive(4096);
		f.model->serviceRetirement();
	}
	CHECK(f.model->safeToRelease());
	CHECK(archive_mock::storage.open.empty());
}

TEST(LoopBindings, cancellationCleanupCloseFailureAbandonsReaderAndStillReleasesSafely) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->requestArchive(0));
	for (unsigned i = 0; i < 20 && f.model->archive().state(0) != ArchiveState::ReadyForPublication; ++i)
		f.model->serviceArchive(4096);
	ArchiveCandidate checkpoint{};
	CHECK(f.model->archive().take(0, checkpoint));
	RestoreId restore{};
	CHECK(Status::Ok == f.model->requestCheckpointRestore(0, checkpoint.identity, restore));
	bool canceledInsideOpen = false;
	archive_mock::storage.yielding = [&](const char* operation) {
		if (!canceledInsideOpen && std::string(operation) == "open") {
			canceledInsideOpen = true;
			archive_mock::storage.failOperation = "close";
			f.model->requestRelease();
		}
	};
	CHECK(Status::Stale == f.driveRestore(restore, 32));
	CHECK(canceledInsideOpen);
	CHECK_EQUAL(1, archive_mock::storage.calls["close"]);
	for (unsigned i = 0; i < 1000 && !f.model->safeToRelease(); ++i) {
		f.model->serviceArchive(4096);
		f.model->serviceRetirement();
	}
	CHECK(storage.checkpointReadIdle());
	CHECK(f.model->safeToRelease());
}

TEST(LoopBindings, terminalCheckpointCloseFailureDoesNotBlockReleaseOrMutateLivePhrase) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0);
	const auto before = f.model->engine().state(0);
	CHECK(Status::Ok == f.model->requestArchive(0));
	for (unsigned i = 0; i < 20 && f.model->archive().state(0) != ArchiveState::ReadyForPublication; ++i)
		f.model->serviceArchive(4096);
	ArchiveCandidate checkpoint{};
	CHECK(f.model->archive().take(0, checkpoint));
	RestoreId restore{};
	CHECK(Status::Ok == f.model->requestCheckpointRestore(0, checkpoint.identity, restore));
	archive_mock::storage.failOperation = "close";
	archive_mock::storage.failure = FR_DISK_ERR;
	const auto failed = f.driveRestore(restore, 512);
	CHECK_EQUAL(int(Status::StorageError), int(failed));
	CHECK(storage.checkpointReadIdle());
	CHECK_EQUAL(before.phraseFrames, f.model->engine().state(0).phraseFrames);
	CHECK_EQUAL(before.playing, f.model->engine().state(0).playing);
	f.model->requestRelease();
	for (unsigned i = 0; i < 1000 && !f.model->safeToRelease(); ++i) {
		f.model->serviceArchive(4096);
		f.model->serviceRetirement();
	}
	CHECK(f.model->safeToRelease());
}

TEST(LoopBindings, oneShotCompletionDuringRestoreBuildIsReflectedByBoundaryCommit) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	auto settings = f.model->engine().settings();
	settings.tracks[0].playback = PlaybackMode::OneShot;
	f.model->dispatch(Command::configure(settings));
	f.record(0);
	CHECK(f.model->engine().state(0).playing);
	CHECK(Status::Ok == f.model->requestArchive(0));
	for (unsigned i = 0; i < 20 && f.model->archive().state(0) != ArchiveState::ReadyForPublication; ++i)
		f.model->serviceArchive(4096);
	ArchiveCandidate checkpoint{};
	CHECK(f.model->archive().take(0, checkpoint));
	RestoreId restore{};
	CHECK(Status::Ok == f.model->requestCheckpointRestore(0, checkpoint.identity, restore));
	CHECK(Status::Busy == f.model->serviceRestore(restore, 2));
	f.render(8); // The one-shot reaches its end while the checkpoint is staged.
	f.render(1); // Consume the audio completion before commit.
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK(Status::Ok == f.driveRestore(restore, 512));
	CHECK(Status::Ok == f.model->commitRestore(restore));
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK(f.model->engine().state(0).hasPhrase);
}

TEST(LoopBindings, transportStartStopPreservesRedoUntilAnEditBegins) {
	Fixture f;
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	for (unsigned i = 0; i < 9; ++i)
		f.render(128);
	f.host.time += GestureRollback::kProvisionalSamples; // Let rollback confirm the overdub before closing it.
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	RestoreId undo{};
	CHECK(f.model->canUndo(0));
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestRestore(0, false, undo)));
	CHECK(Status::Ok == f.driveRestore(undo, 512));
	CHECK(Status::Ok == f.model->commitRestore(undo));
	f.model->dispatch({CommandType::StartAll});
	f.model->dispatch({CommandType::TransportStop});
	RestoreId redo{};
	CHECK(Status::Ok == f.model->requestRestore(0, true, redo));
	CHECK(Status::Ok == f.driveRestore(redo, 512));
	CHECK(Status::Ok == f.model->commitRestore(redo));
}

TEST(LoopBindings, clearThenNewTakeAndOverdubUndoOnlyTouchesTheNewPhrase) {
	Fixture f;
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	for (unsigned i = 0; i < 9; ++i)
		f.render(128);
	f.host.time += GestureRollback::kProvisionalSamples;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	RestoreId oldUndo{};
	CHECK(f.model->canUndo(0));
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestRestore(0, false, oldUndo)));
	CHECK(Status::Ok == f.driveRestore(oldUndo, 512));
	CHECK(Status::Ok == f.model->commitRestore(oldUndo));
	f.model->dispatch({CommandType::TransportStop});
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.host.time += 2 * kSampleRate;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Release, 0}));
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	for (unsigned i = 0; i < 10; ++i)
		f.model->serviceRetirement();
	f.record(0);
	const auto newLength = f.model->engine().state(0).phraseFrames;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	for (unsigned i = 0; i < 9; ++i)
		f.render(128);
	f.host.time += GestureRollback::kProvisionalSamples;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	RestoreId newUndo{};
	CHECK(Status::Ok == f.model->requestRestore(0, false, newUndo));
	CHECK(Status::Ok == f.driveRestore(newUndo, 512));
	CHECK(Status::Ok == f.model->commitRestore(newUndo));
	CHECK_EQUAL(newLength, f.model->engine().state(0).phraseFrames);
}
