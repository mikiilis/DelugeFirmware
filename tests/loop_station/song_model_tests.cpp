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
	std::function<void()> onEnterBoundary;
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
		if (onEnterBoundary)
			onEnterBoundary();
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
	void render(size_t count = 8, Frame input = {100, -100}) {
		std::array<Frame, 128> inputBuffer{}, output{};
		inputBuffer.fill(input);
		model->adapter().finalizeWindow(host.time, count, model->clock());
		for (SlotId id = 0; id < kTrackCount; ++id)
			if (model->clip(id))
				model->adapter().renderDry(id, std::span(inputBuffer).first(count), std::span(output).first(count));
		host.time += count;
	}
	void record(SlotId id, Frame input = {100, -100}) {
		model->serviceRetirement();
		CHECK(Status::Ok == model->ensureBound(id));
		CHECK(Status::Ok == model->dispatch({CommandType::Press, id}));
		render(8, input);
		CHECK(Status::Ok == model->dispatch({CommandType::Press, id}));
	}
	// Run one confirmed history step to its terminal result through the real
	// model-owned foreground service (no view timer).
	HistoryNavigationResult undo(SlotId slot, bool redo) {
		HistoryTarget target{};
		CHECK_EQUAL(int(Status::Ok), int(model->historyTarget(slot, redo, target)));
		RestoreId id{};
		CHECK_EQUAL(int(Status::Ok), int(model->requestHistoryNavigation(target, id)));
		return driveNavigation(id);
	}
	// Begin a save and drive it to a ready pending manifest (not committed).
	void runSave(PersistenceState& persistence) {
		CHECK_EQUAL(int(Status::Ok), int(persistence.requestSave(model.get())));
		for (unsigned i = 0; i < 64 && !persistence.pendingReady(); ++i) {
			model->serviceArchive(4096);
			persistence.serviceArchiveCandidates(*model);
		}
		CHECK(persistence.pendingReady());
	}
	std::array<Generation, kTrackCount> currentRevisions() const {
		std::array<Generation, kTrackCount> revisions{};
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			revisions[slot] = model->contentRevision(slot);
		return revisions;
	}
	// Read an archived checkpoint through the real foreground reader and return
	// its decoded frames (24-bit format values, not the source RAM frames).
	std::vector<Frame> readArchivedFrames(ArchiveStorage& storage, SlotId slot, Generation identity,
	                                      FrameIndex frames) {
		std::vector<Frame> decoded(frames);
		size_t framesRead = 0;
		FrameIndex totalFrames = 0;
		Status status = Status::Busy;
		for (unsigned i = 0; i < 40 && status == Status::Busy; ++i)
			status = storage.readCheckpoint(slot, identity, 0, decoded, framesRead, totalFrames);
		CHECK_EQUAL(int(Status::Ok), int(status));
		CHECK_EQUAL(frames, framesRead);
		CHECK_EQUAL(frames, totalFrames);
		decoded.resize(framesRead);
		storage.cancelCheckpointRead(slot, identity);
		while (!storage.checkpointReadIdle())
			storage.serviceCheckpointReadCleanup();
		return decoded;
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
	// Drive the production foreground owner until the navigation job reaches a
	// terminal state. No UI timer is involved.
	HistoryNavigationResult driveNavigation(RestoreId id) {
		for (unsigned i = 0; i < 5000; ++i) {
			model->serviceHistoryNavigation(128);
			const auto result = model->historyNavigationResult();
			if (result.id.generation == id.generation && !result.pending())
				return result;
		}
		return model->historyNavigationResult();
	}
	// Render one window of a playing phrase and return the exact backing frames.
	std::vector<Frame> backingWindow(SlotId id, size_t count) {
		std::array<Frame, 128> input{}, output{};
		size_t frames = count;
		model->adapter().finalizeWindow(host.time, frames, model->clock());
		model->adapter().renderDry(id, std::span(input).first(frames), std::span(output).first(frames));
		host.time += frames;
		return {output.begin(), output.begin() + frames};
	}
	void expectUniformBacking(SlotId id, size_t count, Frame expected) {
		const auto frames = backingWindow(id, count);
		CHECK_EQUAL(count, frames.size());
		for (const auto& frame : frames) {
			CHECK_EQUAL(expected.l, frame.l);
			CHECK_EQUAL(expected.r, frame.r);
		}
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
	for (unsigned i = 0; i < 200 && !f.model->safeToRelease(); ++i)
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

// Confirmed Undo of an active initial take seals exactly the frames captured
// through confirmation, publishes a truly empty/stopped track (no automatic
// Play/Dub), retains the recording for Redo, and Redo republishes it stopped.
TEST(LoopBindings, ConfirmedActiveInitialUndoRetainsFramesThroughBoundary) {
	Fixture f;
	f.start();
	CHECK(Status::Ok == f.model->ensureBound(0));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK(f.model->engine().state(0).capture);
	CHECK_FALSE(f.model->engine().state(0).playing);
	f.render(8); // Eight captured frames of {100,100}.
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	CHECK(f.model->historyTargetCurrent(target));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	CHECK(id.valid());
	CHECK_FALSE(f.model->engine().state(0).capture);
	const auto result = f.driveNavigation(id);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(result.state));
	CHECK_EQUAL(int(Status::Ok), int(result.status));
	CHECK_EQUAL(id.generation, result.id.generation);
	CHECK_EQUAL(0, result.slot);
	CHECK_FALSE(result.redo);
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK_FALSE(f.model->engine().state(0).capture);
	CHECK_EQUAL(0, f.model->engine().state(0).phraseFrames);
	CHECK(f.model->canRedo(0));
	// Redo from empty restores the same frames, stopped from zero.
	HistoryTarget redo{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, true, redo)));
	RestoreId redoId{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(redo, redoId)));
	const auto redone = f.driveNavigation(redoId);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(redone.state));
	CHECK_EQUAL(int(Status::Ok), int(redone.status));
	CHECK_TRUE(redone.redo);
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK_EQUAL(8, f.model->engine().state(0).phraseFrames);
	CHECK(Status::Ok == f.model->dispatch({CommandType::StartAll}));
	CHECK(f.model->engine().state(0).playing);
	f.expectUniformBacking(0, 8, {100, 100});
}

// A confirmed Undo during an active overdub closes exactly that session and
// removes only its additions, keeping the backing phrase playing. Another track
// is untouched.
TEST(LoopBindings, ConfirmedActiveOverdubUndoRemovesOnlyThatSession) {
	Fixture f;
	f.start();
	f.record(0);
	f.record(1);
	f.expectUniformBacking(0, 8, {100, 100});
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK(f.model->engine().state(0).capture);
	CHECK(f.model->engine().state(0).playing);
	f.render(128); // Adds {100,100} on top of the base over a full pass.
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	CHECK(f.model->historyTargetCurrent(target));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_FALSE(f.model->engine().state(0).capture);
	const auto result = f.driveNavigation(id);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(result.state));
	CHECK_EQUAL(int(Status::Ok), int(result.status));
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->engine().state(0).playing);
	CHECK_EQUAL(8, f.model->engine().state(0).phraseFrames);
	f.expectUniformBacking(0, 8, {100, 100});
	f.expectUniformBacking(1, 8, {100, 100});
}

// An active overdub that captured no samples closes on confirmation and reports
// SourceUnavailable, never falling through to the older initial boundary.
TEST(LoopBindings, ActiveUnchangedOverdubDoesNotUndoInitial) {
	Fixture f;
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK(f.model->engine().state(0).capture);
	// No render: the gate is open but nothing was captured.
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::SourceUnavailable), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_FALSE(id.valid());
	CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
	// The gate is closed, playback continues, and the initial layer is intact.
	CHECK_FALSE(f.model->engine().state(0).capture);
	CHECK(f.model->engine().state(0).playing);
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK_EQUAL(8, f.model->engine().state(0).phraseFrames);
	CHECK(f.model->canUndo(0));
	f.expectUniformBacking(0, 8, {100, 100});
	// The initial boundary is still the next undoable layer.
	HistoryTarget initial{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, initial)));
	RestoreId initialId{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(initial, initialId)));
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.driveNavigation(initialId).state));
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
}

// Opening a fresh initial recording is the new-edit boundary: even a zero-frame
// take invalidates the prior Redo branch, while another track's history is
// untouched.
TEST(LoopBindings, NewInitialTakeInvalidatesRedoAtAdmission) {
	Fixture f;
	f.start();
	f.record(0);
	f.record(1);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, false).state));
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(1, false).state));
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->canRedo(0));
	CHECK(f.model->canRedo(1));
	// Start a fresh initial take on track 0 and close it with zero captured frames.
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK(f.model->engine().state(0).capture && !f.model->engine().state(0).playing);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0})); // Zero-frame closure.
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	CHECK_FALSE(f.model->canRedo(0)); // The old branch is invalidated at admission.
	HistoryTarget redo{};
	CHECK_EQUAL(int(Status::SourceUnavailable), int(f.model->historyTarget(0, true, redo)));
	CHECK(f.model->canRedo(1)); // Another track is unaffected.
}

// Incoming frames inside one capture session must not invalidate a confirmed
// target, so a prompt armed at the start still confirms the same session later.
TEST(LoopBindings, FramesWithinSameCaptureDoNotInvalidateTarget) {
	Fixture f;
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	const auto epoch = target.historyEpoch;
	for (unsigned i = 0; i < 12; ++i)
		f.render(128);
	CHECK_EQUAL(epoch, f.model->engine().historyEpoch(0));
	CHECK(f.model->historyTargetCurrent(target));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.driveNavigation(id).state));
	f.expectUniformBacking(0, 8, {100, 100});
}

// Automatic completion of the armed capture invalidates the old target, so a
// stale confirmation cannot remove the whole just-completed recording.
TEST(LoopBindings, AutomaticCompletionCancelsOldCaptureTarget) {
	Fixture f;
	f.start();
	CHECK(Status::Ok == f.model->ensureBound(0));
	auto settings = f.model->engine().settings();
	settings.tracks[0].recordDurationBars = 1;
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	f.model->updateClock(f.host.time, 1000u << 16, true, 0); // Short bar keeps the take brief.
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK(f.model->engine().state(0).capture);
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	for (unsigned i = 0; i < 400 && f.model->engine().state(0).capture; ++i)
		f.render(128);
	CHECK_FALSE(f.model->engine().state(0).capture);
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK_FALSE(f.model->historyTargetCurrent(target));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Stale), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_FALSE(id.valid());
	// The naturally completed recording is a normal undoable boundary instead.
	CHECK(f.model->canUndo(0));
}

// A confirmed operation progresses through the model-owned foreground service
// with no view timer and commits its audio exactly once.
TEST(LoopBindings, RestoreProgressesWithoutUiTimerAndCommitsOnce) {
	Fixture f;
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	for (unsigned i = 0; i < 9; ++i)
		f.render(128);
	f.host.time += GestureRollback::kProvisionalSamples;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	const auto revisionBefore = f.model->contentRevision(0);
	const auto result = f.driveNavigation(id);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(result.state));
	CHECK_EQUAL(int(Status::Ok), int(result.status));
	CHECK_EQUAL(id.generation, result.id.generation);
	CHECK_EQUAL(revisionBefore + 1, f.model->contentRevision(0));
	f.expectUniformBacking(0, 8, {100, 100});
	// Extra foreground ticks after completion are inert.
	for (unsigned i = 0; i < 8; ++i)
		f.model->serviceHistoryNavigation(128);
	CHECK_EQUAL(revisionBefore + 1, f.model->contentRevision(0));
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.model->historyNavigationResult().state));
}

// Commit Busy retains the job and its ID and retries; checkpoint restore cannot
// enter while a user job owns the single restore admission gate.
TEST(LoopBindings, CommitBusyRetainsJobAndRetries) {
	Fixture f;
	f.start();
	CHECK(Status::Ok == f.model->ensureBound(0));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8);
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	// Queue a quantized Start so the commit sees a pending engine action.
	auto settings = f.model->engine().settings();
	settings.startAction.mode = ActionMode::Synced;
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	CHECK(Status::Ok == f.model->dispatch({CommandType::StartAll}));
	CHECK(f.model->engine().state(0).pending);
	RestoreId checkpoint{};
	CHECK_EQUAL(int(Status::Busy), int(f.model->requestCheckpointRestore(0, 7, checkpoint)));
	f.model->serviceHistoryNavigation(128);
	const auto retained = f.model->historyNavigationResult();
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(retained.state));
	CHECK_EQUAL(id.generation, retained.id.generation);
	// Once the queued action resolves the retained job commits.
	for (unsigned i = 0; i < 400 && f.model->engine().state(0).pending; ++i)
		f.render(128);
	CHECK_FALSE(f.model->engine().state(0).pending);
	const auto result = f.driveNavigation(id);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(result.state));
	CHECK_EQUAL(int(Status::Ok), int(result.status));
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
}

// An ordinary overdub Undo whose reconstruction has already sealed its candidate
// must survive a commit Busy: the retained job retries its commit instead of
// re-reserving the now-sealed candidate and failing.
TEST(LoopBindings, OrdinaryOverdubUndoRetriesCommitBusyAfterSeal) {
	Fixture f;
	f.start();
	f.record(0);                                                     // Initially playing, capture closed.
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0})); // Open a changed overdub.
	for (unsigned i = 0; i < 9; ++i)
		f.render(128);
	f.host.time += GestureRollback::kProvisionalSamples;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));      // Close the overdub boundary.
	CHECK(Status::Ok == f.model->dispatch({CommandType::TransportStop})); // Ordinary Undo on a stopped track.
	CHECK_FALSE(f.model->engine().state(0).playing);
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	const auto revisionBefore = f.model->contentRevision(0);
	// Queue a quantized Start so the commit is Busy once the candidate has sealed.
	auto settings = f.model->engine().settings();
	settings.startAction.mode = ActionMode::Synced;
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	CHECK(Status::Ok == f.model->dispatch({CommandType::StartAll}));
	CHECK(f.model->engine().state(0).pending);
	// Several foreground quanta seal the reconstruction while commit stays Busy.
	for (unsigned i = 0; i < 8; ++i)
		f.model->serviceHistoryNavigation(128);
	const auto retained = f.model->historyNavigationResult();
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(retained.state));
	CHECK_EQUAL(id.generation, retained.id.generation);
	// Resolve the pending transport action; the retained job commits exactly once.
	for (unsigned i = 0; i < 400 && f.model->engine().state(0).pending; ++i)
		f.render(128);
	CHECK_FALSE(f.model->engine().state(0).pending);
	const auto result = f.driveNavigation(id);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(result.state));
	CHECK_EQUAL(int(Status::Ok), int(result.status));
	CHECK_EQUAL(revisionBefore + 1, f.model->contentRevision(0));
	CHECK(f.model->engine().state(0).hasPhrase);
	f.expectUniformBacking(0, 8, {100, 100});
}

// Reentrant foreground service cannot service or commit the job twice.
TEST(LoopBindings, ReentrantServiceDoesNotDoublePublish) {
	Fixture f;
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	for (unsigned i = 0; i < 9; ++i)
		f.render(128);
	f.host.time += GestureRollback::kProvisionalSamples;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	const auto revisionBefore = f.model->contentRevision(0);
	bool reentered = false;
	f.host.onEnterBoundary = [&] {
		if (!reentered) {
			reentered = true;
			f.model->serviceHistoryNavigation(128); // Must be rejected by the service guard.
		}
	};
	const auto result = f.driveNavigation(id);
	f.host.onEnterBoundary = {};
	CHECK(reentered);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(result.state));
	CHECK_EQUAL(int(Status::Ok), int(result.status));
	CHECK_EQUAL(revisionBefore + 1, f.model->contentRevision(0));
	f.expectUniformBacking(0, 8, {100, 100});
}

// A supported root-view handoff must not abort an accepted Undo: the model-owned
// job keeps progressing and publishes once.
TEST(LoopBindings, AcceptedNavigationSurvivesRootViewExit) {
	Fixture f;
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8);
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(f.model->historyNavigationResult().state));
	const auto revision = f.model->contentRevision(0);
	// Leaving the Loop Station root for supported navigation is not invalidation.
	CHECK(Status::Ok == f.model->requestViewExit());
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(f.model->historyNavigationResult().state));
	const auto result = f.driveNavigation(id);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(result.state));
	CHECK_EQUAL(int(Status::Ok), int(result.status));
	CHECK_EQUAL(revision + 1, f.model->contentRevision(0));
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->engine().state(0).playing);
	CHECK_EQUAL(8, f.model->engine().state(0).phraseFrames);
	f.expectUniformBacking(0, 8, {100, 100});
}

// Selecting a track is not an audio invalidation; an accepted job keeps running.
TEST(LoopBindings, AcceptedNavigationSurvivesSelection) {
	Fixture f;
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8);
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Select, 0}));
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(f.model->historyNavigationResult().state));
	const auto result = f.driveNavigation(id);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(result.state));
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->engine().state(0).playing);
	CHECK_EQUAL(8, f.model->engine().state(0).phraseFrames);
	f.expectUniformBacking(0, 8, {100, 100});
}

// A pad release the engine treats as a no-op must not cancel accepted work: the
// confirmed closure already cleared the hold, so this release changes nothing.
TEST(LoopBindings, ObsoleteReleaseDoesNotCancelAcceptedNavigation) {
	Fixture f;
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8);
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_FALSE(f.model->engine().state(0).capture);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Release, 0}));
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(f.model->historyNavigationResult().state));
	const auto result = f.driveNavigation(id);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(result.state));
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->engine().state(0).playing);
	CHECK_EQUAL(8, f.model->engine().state(0).phraseFrames);
	f.expectUniformBacking(0, 8, {100, 100});
}

// An actual destructive Clear is a real invalidation and cancels accepted work.
TEST(LoopBindings, ClearCancelsAcceptedNavigation) {
	Fixture f;
	f.start();
	CHECK(Status::Ok == f.model->ensureBound(0));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8);
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(f.model->historyNavigationResult().state));
	// Arm a hold on the now-stopped phrase, then release to Clear.
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(f.model->historyNavigationResult().state));
	f.host.time += 2 * kSampleRate;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Release, 0}));
	CHECK_EQUAL(int(HistoryNavigationState::Failed), int(f.model->historyNavigationResult().state));
	for (unsigned i = 0; i < 8; ++i)
		f.model->serviceHistoryNavigation(128);
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
}

// Known-incomplete before-images are observable before closure: confirmation must
// leave an active overdub recording instead of stopping it to report the failure.
TEST(LoopBindings, KnownIncompleteOverdubLeavesGateOpen) {
	Fixture f;
	f.start();
	CHECK(Status::Ok == f.model->ensureBound(0));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	for (unsigned i = 0; i < 4300; ++i)
		f.render(128);
	CHECK(f.model->engine().state(0).capture);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0})); // Finish the long base.
	CHECK(f.model->engine().state(0).playing);
	const auto baseFrames = f.model->engine().state(0).phraseFrames;
	CHECK(baseFrames > 1024u * (kPageBytes / sizeof(Frame)));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0})); // Open overdub.
	HistoryTarget armed{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, armed)));
	// Overwrite every page once; before-images are lost under page pressure.
	for (unsigned i = 0; i < 4300; ++i) {
		if (f.model->historyTarget(0, false, armed) != Status::Ok)
			break;
		f.render(128);
	}
	CHECK_EQUAL(int(Status::SourceUnavailable), int(f.model->historyTarget(0, false, armed)));
	CHECK(f.model->engine().state(0).capture);
	// The target armed before the loss is still current (frames do not invalidate),
	// yet confirmation must leave the gate running on this known-unrestorable layer.
	CHECK(f.model->historyTargetCurrent(armed));
	RestoreId id{};
	CHECK_EQUAL(int(Status::SourceUnavailable), int(f.model->requestHistoryNavigation(armed, id)));
	CHECK_FALSE(id.valid());
	CHECK(f.model->engine().state(0).capture);
	CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
}

// Known-exhausted detached capacity is observable before closure too: confirming
// must not stop the recording only to report NoMemory.
TEST(LoopBindings, KnownNoMemoryAdmissionLeavesOverdubGateOpen) {
	Fixture f;
	f.start();
	f.record(1);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 1}));
	f.render(8);
	f.host.time += GestureRollback::kProvisionalSamples;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 1})); // Track 1 boundary.
	unsigned retired = 0;
	for (unsigned i = 0; i < 40; ++i) {
		RestoreId candidate{};
		const auto requested = f.model->requestRestore(1, false, candidate);
		if (requested != Status::Ok) {
			CHECK_EQUAL(int(Status::NoMemory), int(requested));
			break;
		}
		const auto serviced = f.model->serviceRestore(candidate, 512);
		if (serviced != Status::Ok) {
			CHECK_EQUAL(int(Status::NoMemory), int(serviced));
			break;
		}
		f.model->cancelRestore(1);
		++retired;
	}
	CHECK(retired >= 8);
	// Track 0 changed overdub, without draining the exhausted detached records.
	CHECK(Status::Ok == f.model->ensureBound(0));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8);
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::NoMemory), int(f.model->historyTarget(0, false, target)));
	CHECK(f.model->engine().state(0).capture);
	RestoreId id{};
	CHECK_EQUAL(int(Status::NoMemory), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_FALSE(id.valid());
	CHECK(f.model->engine().state(0).capture); // Gate stays open.
	CHECK_EQUAL(int(HistoryNavigationState::Idle), int(f.model->historyNavigationResult().state));
}

// A restore that fails after confirmation already closed the capture - here the
// just-closed boundary is evicted by descriptor pressure - keeps the cumulative
// captured audio, leaves capture closed, reports the failure, and does not
// substitute an older layer.
TEST(LoopBindings, LaterReconstructionFailureRetainsAudioAfterClosure) {
	Fixture f;
	f.start();
	f.record(1);
	CHECK(Status::Ok == f.model->ensureBound(0));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8); // One changed overdub pass: {200,200}.
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_FALSE(f.model->engine().state(0).capture);
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(f.model->historyNavigationResult().state));
	// Descriptor pressure on track 1 evicts track 0's just-closed boundary while the
	// accepted job is applying; this is not knowable before capture closure.
	for (unsigned i = 0; i < 200; ++i) {
		f.host.time += GestureRollback::kProvisionalSamples;
		CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 1}));
		f.render(1);
		f.host.time += GestureRollback::kProvisionalSamples;
		CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 1}));
	}
	// The just-closed boundary is gone, so the accepted job can only fail.
	CHECK_FALSE(f.model->canUndo(0));
	const auto result = f.driveNavigation(id);
	CHECK_EQUAL(int(HistoryNavigationState::Failed), int(result.state));
	CHECK_EQUAL(int(Status::SourceUnavailable), int(result.status));
	// Capture closed, cumulative additions preserved, no older layer removed.
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->engine().state(0).playing);
	CHECK_EQUAL(8, f.model->engine().state(0).phraseFrames);
	f.expectUniformBacking(0, 8, {200, 200});
}

// A new capture on the same track cancels an accepted job before it can publish
// anything, so a stale confirmed Undo cannot edit the fresh session.
TEST(LoopBindings, NewCaptureCancelsApplyingJobBeforePublication) {
	Fixture f;
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8); // One overdubbed pass over the eight-frame base.
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_FALSE(f.model->engine().state(0).capture);
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(f.model->historyNavigationResult().state));
	const auto revision = f.model->contentRevision(0);
	// Opening a fresh overdub on the same track must cancel the accepted job.
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK_EQUAL(int(HistoryNavigationState::Failed), int(f.model->historyNavigationResult().state));
	CHECK(f.model->engine().state(0).capture);
	// Servicing the cancelled job can never publish the old removal.
	for (unsigned i = 0; i < 8; ++i)
		f.model->serviceHistoryNavigation(128);
	CHECK_EQUAL(revision, f.model->contentRevision(0));
	CHECK(f.model->engine().state(0).capture);
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->engine().state(0).playing);
}

// An observed running/stopped transport change (not a UI button) invalidates a
// confirmed target before it can act.
TEST(LoopBindings, ExternalTransportTransitionInvalidatesTarget) {
	Fixture f;
	f.start();
	f.model->updateClock(f.host.time, 120u << 16, true, 0);
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	for (unsigned i = 0; i < 3; ++i)
		f.render(128);
	f.host.time += GestureRollback::kProvisionalSamples;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	f.model->updateClock(f.host.time, 120u << 16, true, 0); // Running -> running is not a change.
	CHECK(f.model->historyTargetCurrent(target));
	f.model->updateClock(f.host.time, 120u << 16, false, 0); // Running -> stopped invalidates.
	CHECK_FALSE(f.model->historyTargetCurrent(target));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Stale), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_FALSE(id.valid());
}

// A loaded checkpoint is the base recording of one new volatile history chain:
// exactly one undoable base. Undo detaches it to empty, Redo republishes the same
// whole recording stopped, and a later failed checkpoint read adds no second base.
TEST(LoopBindings, LoadedCheckpointIsOneUndoableBase) {
	archive_mock::storage = {};
	ArchiveCandidate checkpoint{};
	{
		FatFsArchiveStorage writerStorage;
		Fixture source;
		source.model->archive().setStorage(&writerStorage);
		source.start();
		source.record(0, {256, -256}); // 24-bit-safe so the reload is exact.
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
	CHECK(Status::Ok == loaded.driveRestore(restore, 512));
	CHECK(Status::Ok == loaded.model->commitRestore(restore));
	CHECK(loaded.model->engine().state(0).hasPhrase);
	CHECK_FALSE(loaded.model->engine().state(0).playing);
	// Exactly one base boundary: immediately undoable, with no Redo branch yet.
	CHECK(loaded.model->canUndo(0));
	CHECK_FALSE(loaded.model->canRedo(0));

	// A failed later checkpoint read must not register a partial second base.
	const char* path = readerStorage.candidatePath(checkpoint.slot, checkpoint.identity);
	CHECK(path != nullptr);
	archive_mock::storage.files[path][0] = std::byte{'X'};
	RestoreId failed{};
	CHECK(Status::Ok == loaded.model->requestCheckpointRestore(0, checkpoint.identity, failed));
	CHECK_EQUAL(int(Status::StorageError), int(loaded.driveRestore(failed, 512)));
	loaded.drainStorage();
	CHECK(loaded.model->canUndo(0));
	CHECK_FALSE(loaded.model->canRedo(0));

	// Undo to empty retains the recording for Redo; one step empties the track.
	const auto undone = loaded.undo(0, false);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(undone.state));
	CHECK_FALSE(loaded.model->engine().state(0).hasPhrase);
	CHECK_FALSE(loaded.model->canUndo(0));
	CHECK(loaded.model->canRedo(0));
	// Redo republishes the same recording, stopped from zero.
	const auto redone = loaded.undo(0, true);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(redone.state));
	CHECK(loaded.model->engine().state(0).hasPhrase);
	CHECK(redone.redo);
	CHECK_EQUAL(checkpoint.frames, loaded.model->engine().state(0).phraseFrames);
	CHECK(Status::Ok == loaded.model->dispatch({CommandType::StartAll}));
	// Source::Left captures the left channel into both sides; 256 survives 24-bit
	// exactly, so this is format-appropriate rather than a 32-bit identity claim.
	loaded.expectUniformBacking(0, checkpoint.frames, {256, 256});
	CHECK(loaded.model->canUndo(0));
}

// An accepted checkpoint restore is accepted background work: a harmless root
// view exit must not abandon it, while an actual teardown still cancels it.
TEST(LoopBindings, AcceptedCheckpointRestoreSurvivesViewExitButNotTeardown) {
	archive_mock::storage = {};
	ArchiveCandidate checkpoint{};
	{
		FatFsArchiveStorage writerStorage;
		Fixture source;
		source.model->archive().setStorage(&writerStorage);
		source.start();
		source.record(0, {256, -256});
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
	// Partially read the real checkpoint, then leave the root view mid-read.
	loaded.model->serviceArchive(50);
	CHECK(loaded.model->checkpointRestorePending(checkpoint.slot));
	CHECK(Status::Ok == loaded.model->requestViewExit());
	CHECK(loaded.model->checkpointRestorePending(checkpoint.slot)); // Not abandoned.
	CHECK(Status::Ok == loaded.driveRestore(restore, 512));
	CHECK(Status::Ok == loaded.model->commitRestore(restore));
	CHECK(loaded.model->engine().state(0).hasPhrase);
	CHECK_FALSE(loaded.model->engine().state(0).playing);
	CHECK(loaded.model->canUndo(0)); // Exactly one base boundary...
	CHECK_FALSE(loaded.model->canRedo(0));
	CHECK_EQUAL(checkpoint.frames, loaded.model->engine().state(0).phraseFrames);
	CHECK(Status::Ok == loaded.model->dispatch({CommandType::StartAll}));
	loaded.expectUniformBacking(0, checkpoint.frames, {256, 256});

	// An actual teardown still cancels an accepted checkpoint restore.
	RestoreId second{};
	CHECK(Status::Ok == loaded.model->requestCheckpointRestore(0, checkpoint.identity, second));
	loaded.model->serviceArchive(50);
	CHECK(loaded.model->checkpointRestorePending(0));
	loaded.model->requestRelease();
	CHECK_EQUAL(int(Status::Stale), int(loaded.driveRestore(second, 512)));
	CHECK_FALSE(loaded.model->checkpointRestorePending(0));
}

// Undoing the only layer to empty and saving must omit the old audio checkpoint
// while keeping bound settings; a reload must not resurrect the removed recording.
TEST(LoopBindings, SaveAfterUndoToEmptyDoesNotResurrectRecordingOnReload) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	auto settings = f.model->engine().settings();
	settings.tracks[0].transposeCents = 1200;
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	f.record(0);

	PersistenceState persistence;
	CHECK(Status::Ok == persistence.installLoaded(defaultSettings(), Manifest{}));
	f.runSave(persistence);
	CHECK(Status::Ok == persistence.commitPendingAfterDocumentVerified());
	CHECK_EQUAL(1, persistence.committedManifest().phraseCount);
	CHECK_EQUAL(0, persistence.committedManifest().phrases[0].slot);
	const auto savedIdentity = persistence.committedManifest().phrases[0].identity;
	f.model->markAudioSaved(0, persistence.committedAudioRevision(0));
	CHECK_FALSE(f.model->hasUnsavedAudio());

	// Undo the only layer to empty.
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, false).state));
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->canRedo(0));

	// Saving the empty bound track omits the existing checkpoint but keeps settings.
	f.runSave(persistence);
	CHECK(Status::Ok == persistence.commitPendingAfterDocumentVerified());
	CHECK_EQUAL(0, persistence.committedManifest().phraseCount);
	CHECK_EQUAL(2, persistence.committedManifest().generation);
	CHECK_EQUAL(1200, persistence.committedSettings().tracks[0].transposeCents);
	CHECK_EQUAL(int(f.model->engine().settings().recordAction), int(persistence.committedSettings().recordAction));
	SettingsDocument document{};
	CHECK(Status::Ok == persistence.encodeForSave(document));
	CHECK_EQUAL(0, document.phraseCount);
	CHECK_EQUAL(2, document.manifestGeneration);
	CHECK_EQUAL(1200, document.tracks[0].transposeCents);

	// A reload admits no phrase and does not resurrect the removed recording.
	Settings reloadedSettings{};
	Manifest reloadedManifest{};
	CHECK(Status::Ok == Persistence::decode(document, reloadedSettings, reloadedManifest));
	CHECK_EQUAL(0, reloadedManifest.phraseCount);
	CHECK_EQUAL(1200, reloadedSettings.tracks[0].transposeCents);
	PersistenceState reloaded;
	CHECK(Status::Ok == reloaded.installLoaded(reloadedSettings, reloadedManifest));
	CHECK(Status::Ok == reloaded.prepareLoadAdmission(MemoryProfile::Preferred));
	CHECK_EQUAL(0, reloaded.loadAdmission().count);
	CHECK_FALSE(reloaded.loadAdmission().admitted[0]);
	CHECK(savedIdentity != 0);

	// A fresh model loading the empty document has no hidden Undo/Redo, while the
	// bound settings survive the reload.
	Fixture reloadedModel;
	reloadedModel.start();
	CHECK(Status::Ok == reloadedModel.model->dispatch(Command::configure(reloadedSettings)));
	CHECK(Status::Ok == reloadedModel.model->ensureBound(0));
	CHECK_FALSE(reloadedModel.model->engine().state(0).hasPhrase);
	CHECK_FALSE(reloadedModel.model->canUndo(0));
	CHECK_FALSE(reloadedModel.model->canRedo(0));
	CHECK_EQUAL(1200, reloadedModel.model->engine().settings().tracks[0].transposeCents);
}

// Saving an undone-empty bound track must not retire its volatile Redo branch,
// and that Redo must not become durable in the saved manifest.
TEST(LoopBindings, SaveDoesNotDiscardVolatileInitialRedo) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0, {256, -256});
	PersistenceState persistence;
	CHECK(Status::Ok == persistence.installLoaded(defaultSettings(), Manifest{}));
	f.runSave(persistence);
	CHECK(Status::Ok == persistence.commitPendingAfterDocumentVerified());
	CHECK_EQUAL(1, persistence.committedManifest().phraseCount);

	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, false).state));
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->canRedo(0));

	// Save the now-empty track. The Redo branch survives as volatile, not durable.
	f.runSave(persistence);
	CHECK(Status::Ok == persistence.commitPendingAfterDocumentVerified());
	CHECK_EQUAL(0, persistence.committedManifest().phraseCount);
	CHECK(f.model->canRedo(0));

	// The retained volatile Redo still republishes the exact recording.
	const auto redone = f.undo(0, true);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(redone.state));
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK_EQUAL(8, f.model->engine().state(0).phraseFrames);
	CHECK(Status::Ok == f.model->dispatch({CommandType::StartAll}));
	f.expectUniformBacking(0, 8, {256, 256});
}

// A save started before Undo captured the earlier content revision; committing it
// after the Undo must not mark the later revision saved, and the committed
// manifest keeps the original immutable checkpoint reference.
TEST(LoopBindings, OldSnapshotCannotMarkLaterUndoStateSaved) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0, {256, -256});
	PersistenceState persistence;
	CHECK(Status::Ok == persistence.installLoaded(defaultSettings(), Manifest{}));
	f.runSave(persistence);
	const auto savedRevision = f.model->contentRevision(0);
	const auto savedIdentity = persistence.pendingManifest().phrases[0].identity;

	// Undo while the save is still pending.
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, false).state));
	CHECK(f.model->contentRevision(0) != savedRevision);

	const auto current = f.currentRevisions();
	CHECK(Status::Ok == persistence.commitPendingAfterDocumentVerified(current));
	CHECK_EQUAL(int(SaveState::Unsaved), int(persistence.saveState()));
	// The committed baseline is the old snapshot revision, never the later one.
	CHECK(persistence.hasCommittedAudioRevision(0));
	CHECK_EQUAL(savedRevision, persistence.committedAudioRevision(0));
	CHECK_EQUAL(1, persistence.committedManifest().phraseCount);
	CHECK_EQUAL(savedIdentity, persistence.committedManifest().phrases[0].identity);
	// Marking the model with that revision cannot clear the later Undo edit.
	f.model->markAudioSaved(0, persistence.committedAudioRevision(0));
	CHECK(f.model->hasUnsavedAudio());
}

// While a save is pending, Undo then Redo swaps the live provider; the frozen
// snapshot still reads its original frames and cannot mark the Redo state saved.
TEST(LoopBindings, RedoDuringSaveRetainsImmutableSnapshot) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	f.record(0, {256, -256});
	PersistenceState persistence;
	CHECK(Status::Ok == persistence.installLoaded(defaultSettings(), Manifest{}));
	f.runSave(persistence);
	const auto savedRevision = f.model->contentRevision(0);
	const auto staged = persistence.pendingManifest().phrases[0];
	CHECK_EQUAL(0, staged.slot);
	CHECK_EQUAL(8, staged.frames);

	// Undo then Redo while the save is still pending.
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, false).state));
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, true).state));
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->contentRevision(0) != savedRevision);

	// The immutable snapshot still reads its original 24-bit frames exactly.
	const auto frames = f.readArchivedFrames(storage, staged.slot, staged.identity, staged.frames);
	for (const auto& frame : frames) {
		CHECK_EQUAL(256, frame.l);
		CHECK_EQUAL(256, frame.r);
	}
	const auto current = f.currentRevisions();
	CHECK(Status::Ok == persistence.commitPendingAfterDocumentVerified(current));
	CHECK_EQUAL(int(SaveState::Unsaved), int(persistence.saveState()));
	CHECK_EQUAL(savedRevision, persistence.committedAudioRevision(0));
	CHECK_EQUAL(1, persistence.committedManifest().phraseCount);
	CHECK_EQUAL(staged.identity, persistence.committedManifest().phrases[0].identity);
}

// A save started from an undone replacement version, then Redo'd before the
// archive finishes, must still publish: the completed immutable candidate no
// longer depends on the retired RAM source remaining live.
TEST(LoopBindings, SaveStartedBetweenUndoAndRedoPublishesAfterRetirement) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	auto settings = f.model->engine().settings();
	settings.tracks[0].source = Source::Stereo; // Distinct left/right frames.
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	f.record(0, {256, -512});
	// One changed overdub boundary, then Undo it so the live replacement is I and
	// the Redo branch retains I+A.
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8, {256, -512});
	f.host.time += GestureRollback::kProvisionalSamples;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, false).state));
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->canRedo(0));

	PersistenceState persistence;
	CHECK(Status::Ok == persistence.installLoaded(defaultSettings(), Manifest{}));
	const auto savedRevision = f.model->contentRevision(0);
	CHECK(Status::Ok == persistence.requestSave(f.model.get()));
	CHECK_EQUAL(int(ArchiveState::Pending), int(f.model->archive().state(0)));

	// Redo while the save is pending: this retires the very record the snapshot owns.
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, true).state));
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->contentRevision(0) != savedRevision);

	for (unsigned i = 0; i < 64 && !persistence.pendingReady(); ++i) {
		f.model->serviceArchive(4096);
		persistence.serviceArchiveCandidates(*f.model);
	}
	CHECK(persistence.pendingReady());
	const auto staged = persistence.pendingManifest().phrases[0];
	CHECK_EQUAL(0, staged.slot);
	CHECK_EQUAL(8, staged.frames);
	const auto frames = f.readArchivedFrames(storage, staged.slot, staged.identity, staged.frames);
	for (const auto& frame : frames) {
		CHECK_EQUAL(256, frame.l);
		CHECK_EQUAL(-512, frame.r);
	}
	const auto current = f.currentRevisions();
	CHECK(Status::Ok == persistence.commitPendingAfterDocumentVerified(current));
	CHECK_EQUAL(int(SaveState::Unsaved), int(persistence.saveState())); // Newer Redo state is dirty.
	CHECK_EQUAL(savedRevision, persistence.committedAudioRevision(0));
	CHECK_EQUAL(1, persistence.committedManifest().phraseCount);
	CHECK_EQUAL(staged.identity, persistence.committedManifest().phrases[0].identity);
	// The retired source's pinned pages are eventually released.
	for (unsigned i = 0; i < 200 && (f.model->archive().idle() == false || f.model->engine().state(0).capture); ++i)
		f.model->serviceArchive(4096);
	for (unsigned i = 0; i < 200; ++i)
		f.model->serviceRetirement();
}

// Replacing the Song while a confirmed history operation is applying cancels the
// accepted job before any late publication; the live phrase is untouched.
TEST(LoopBindings, LoadCancelsApplyingHistoryBeforeNewSongPublication) {
	Fixture f;
	f.start();
	f.record(0);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8); // One changed overdub pass.
	HistoryTarget target{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->historyTarget(0, false, target)));
	RestoreId id{};
	CHECK_EQUAL(int(Status::Ok), int(f.model->requestHistoryNavigation(target, id)));
	CHECK_FALSE(f.model->engine().state(0).capture);
	CHECK_EQUAL(int(HistoryNavigationState::Applying), int(f.model->historyNavigationResult().state));
	const auto revision = f.model->contentRevision(0);
	const auto frames = f.model->engine().state(0).phraseFrames;

	// The load confirms discarding the current volatile audio; the accepted job
	// must be cancelled before the new Song can publish anything.
	const auto confirmed = f.currentRevisions();
	const auto mask = f.model->captureActivityMask();
	const auto replacement = f.model->requestSongReplacement(true, confirmed, mask);
	CHECK_EQUAL(int(HistoryNavigationState::Failed), int(f.model->historyNavigationResult().state));
	CHECK_EQUAL(int(Status::Cancelled), int(f.model->historyNavigationResult().status));
	for (unsigned i = 0; i < 16; ++i)
		f.model->serviceHistoryNavigation(128);
	CHECK_EQUAL(revision, f.model->contentRevision(0));
	CHECK_EQUAL(frames, f.model->engine().state(0).phraseFrames);
	CHECK(f.model->engine().state(0).hasPhrase);
	CHECK(replacement == Status::Busy || replacement == Status::Ok);

	// Teardown drains bounded retirement and never publishes the canceled removal.
	for (unsigned i = 0; i < 1000 && !f.model->safeToRelease(); ++i) {
		f.model->serviceRetirement();
		f.model->serviceArchive(4096);
	}
	CHECK(f.model->safeToRelease());
	CHECK_EQUAL(revision, f.model->contentRevision(0));
	CHECK_EQUAL(frames, f.model->engine().state(0).phraseFrames);
}

// A real in-flight archive keeps its acquired snapshot across initial Undo/Redo:
// the frozen SnapshotId is never finished before navigation, and once it
// progresses it still writes the original stereo frames.
TEST(LoopBindings, InFlightSaveSnapshotSurvivesInitialUndoAndRedo) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	auto settings = f.model->engine().settings();
	settings.tracks[0].source = Source::Stereo; // Distinct left/right frames.
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	f.record(0, {256, -512});

	PersistenceState persistence;
	CHECK(Status::Ok == persistence.installLoaded(defaultSettings(), Manifest{}));
	const auto savedRevision = f.model->contentRevision(0);
	CHECK(Status::Ok == persistence.requestSave(f.model.get()));
	CHECK_EQUAL(int(ArchiveState::Pending), int(f.model->archive().state(0)));

	// Navigate while the save is genuinely in flight (never serviced).
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, false).state));
	CHECK_FALSE(f.model->engine().state(0).hasPhrase);
	CHECK(f.model->archive().state(0) != ArchiveState::Failed);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, true).state));
	CHECK(f.model->engine().state(0).hasPhrase);

	// Now it progresses and reads the frozen original frames.
	for (unsigned i = 0; i < 64 && !persistence.pendingReady(); ++i) {
		f.model->serviceArchive(4096);
		persistence.serviceArchiveCandidates(*f.model);
	}
	CHECK(persistence.pendingReady());
	const auto staged = persistence.pendingManifest().phrases[0];
	CHECK_EQUAL(0, staged.slot);
	CHECK_EQUAL(8, staged.frames);
	const auto frames = f.readArchivedFrames(storage, staged.slot, staged.identity, staged.frames);
	for (const auto& frame : frames) {
		CHECK_EQUAL(256, frame.l);
		CHECK_EQUAL(-512, frame.r);
	}
	const auto current = f.currentRevisions();
	CHECK(Status::Ok == persistence.commitPendingAfterDocumentVerified(current));
	CHECK_EQUAL(int(SaveState::Unsaved), int(persistence.saveState()));
	CHECK_EQUAL(savedRevision, persistence.committedAudioRevision(0));
	CHECK_EQUAL(1, persistence.committedManifest().phraseCount);
}

// The same acquired snapshot survives an ordinary overdub Undo/Redo, which
// detaches/replaces the provider while retaining the original record.
TEST(LoopBindings, InFlightSaveSnapshotSurvivesOverdubUndoAndRedo) {
	archive_mock::storage = {};
	FatFsArchiveStorage storage;
	Fixture f;
	f.model->archive().setStorage(&storage);
	f.start();
	auto settings = f.model->engine().settings();
	settings.tracks[0].source = Source::Stereo;
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	f.record(0, {256, -512});

	PersistenceState persistence;
	CHECK(Status::Ok == persistence.installLoaded(defaultSettings(), Manifest{}));
	CHECK(Status::Ok == persistence.requestSave(f.model.get()));
	CHECK_EQUAL(int(ArchiveState::Pending), int(f.model->archive().state(0)));

	// Open and close one changed overdub pass while the save stays in flight.
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	f.render(8, {256, -512});
	f.host.time += GestureRollback::kProvisionalSamples;
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, 0}));
	CHECK(f.model->engine().state(0).playing);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, false).state));
	CHECK(f.model->archive().state(0) != ArchiveState::Failed);
	CHECK_EQUAL(int(HistoryNavigationState::Complete), int(f.undo(0, true).state));
	CHECK(f.model->engine().state(0).hasPhrase);

	for (unsigned i = 0; i < 64 && !persistence.pendingReady(); ++i) {
		f.model->serviceArchive(4096);
		persistence.serviceArchiveCandidates(*f.model);
	}
	CHECK(persistence.pendingReady());
	const auto staged = persistence.pendingManifest().phrases[0];
	const auto frames = f.readArchivedFrames(storage, staged.slot, staged.identity, staged.frames);
	for (const auto& frame : frames) {
		CHECK_EQUAL(256, frame.l);
		CHECK_EQUAL(-512, frame.r);
	}
}
