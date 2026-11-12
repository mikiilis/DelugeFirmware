#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/archive_storage.h"
#include "deluge/loop_station/song_model.h"
#include "mocks/audio_clip.h"
#include "mocks/storage.h"
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

using namespace loop_station;
namespace {
struct Allocator : ReservationAllocator {
	void* allocatePinnedLowSpeed(size_t size) override {
		if (onAllocate)
			onAllocate();
		return std::malloc(size);
	}
	void deallocate(void* ptr) override { std::free(ptr); }
	std::function<void()> onAllocate;
};
struct Host : BindingHost {
	AudioClip* allocateClip() override {
		auto* clip = new AudioClip;
		clips.push_back(clip);
		return clip;
	}
	Status prepareClip(AudioClip*, SlotId) override {
		if (onPrepare)
			onPrepare();
		return Status::Ok;
	}
	Status publishClip(AudioClip*) override { return Status::Ok; }
	void discardClip(AudioClip* clip) override {
		clips.erase(std::remove(clips.begin(), clips.end(), clip), clips.end());
		delete clip;
	}
	Status attach(AudioClip* clip, AudioAdapter* adapter, SlotId) override {
		clip->adapter = adapter;
		return Status::Ok;
	}
	bool enterBoundary() override { return false; }
	void leaveBoundary(bool) override {}
	void registerAudio(AudioAdapter* adapter, const ClockMap*) override { registered = adapter; }
	SampleClock now() const override { return time; }
	AudioAdapter* registered = nullptr;
	SampleClock time = 0;
	std::vector<AudioClip*> clips;
	std::function<void()> onPrepare;
	~Host() override {
		for (auto* clip : clips)
			delete clip;
	}
};
struct Fixture {
	Allocator allocator;
	Host host;
	SongModel model{allocator, host};
	Fixture(bool startImmediately = true) {
		if (startImmediately)
			start();
	}
	void start() {
		CHECK(Status::Ok == model.start());
		auto settings = defaultSettings();
		settings.startAction.mode = settings.stopAction.mode = ActionMode::Immediate;
		CHECK(Status::Ok == model.dispatch(Command::configure(settings)));
	}
	~Fixture() {
		model.requestRelease();
		for (unsigned i = 0; i < 1000 && !model.safeToRelease(); ++i) {
			model.serviceRetirement();
			model.serviceArchive(4096);
		}
	}
	void render(size_t frames) {
		std::array<Frame, 64> in{}, out{};
		in.fill({100, -100});
		model.adapter().finalizeWindow(host.time, frames, model.clock());
		for (SlotId id = 0; id < kTrackCount; ++id)
			if (model.clip(id))
				model.adapter().renderDry(id, std::span(in).first(frames), std::span(out).first(frames));
		host.time += frames;
	}
	void take(SlotId id) {
		CHECK(Status::Ok == model.ensureBound(id));
		CHECK(Status::Ok == model.dispatch({CommandType::Press, id}));
		render(16);
		CHECK(Status::Ok == model.dispatch({CommandType::Press, id}));
	}
};
} // namespace

TEST_GROUP(LoopLifecycle){};
TEST(LoopLifecycle, transportStopsNowAndClosesCaptureGate) {
	Fixture f;
	f.take(0);
	CHECK(f.model.engine().state(0).playing);
	CHECK(Status::Ok == f.model.requestTransportStop());
	CHECK(!f.model.engine().state(0).playing);
	CHECK(!f.model.engine().state(0).capture);
	CHECK(Status::Ok == f.model.dispatch({CommandType::Press, 0}));
	CHECK(Status::Ok == f.model.dispatch({CommandType::Release, 0}));
	CHECK(f.model.engine().state(0).playing);
	CHECK(Status::Ok == f.model.dispatch({CommandType::Press, 0}));
	CHECK(f.model.engine().state(0).capture);
	CHECK(Status::Ok == f.model.requestTransportStop());
	CHECK(!f.model.engine().state(0).capture);
	CHECK(!f.model.engine().state(0).playing);
}
TEST(LoopLifecycle, replacementDuringYieldingClipSetupCannotPublishLateAttachment) {
	Fixture f;
	Status replacement = Status::Ok;
	f.host.onPrepare = [&] { replacement = f.model.requestSongReplacement(true); };
	CHECK(Status::Stale == f.model.ensureBound(0));
	CHECK(Status::Busy == replacement);
	CHECK(!f.model.clip(0));
	CHECK(Status::Ok == f.model.requestSongReplacement(true));
}
TEST(LoopLifecycle, replacementCancelsAndDrainsActiveCheckpointReader) {
	archive_mock::storage = {};
	ArchiveCandidate checkpoint{};
	{
		FatFsArchiveStorage writer;
		Fixture source;
		source.model.archive().setStorage(&writer);
		source.take(0);
		CHECK(Status::Ok == source.model.requestArchive(0));
		for (unsigned i = 0; i < 32 && source.model.archive().state(0) != ArchiveState::ReadyForPublication; ++i)
			source.model.serviceArchive(4096);
		CHECK(source.model.archive().take(0, checkpoint));
	}
	FatFsArchiveStorage reader;
	Fixture f;
	f.model.archive().setStorage(&reader);
	CHECK(Status::Ok == reader.registerPublishedFile(checkpoint.slot, checkpoint.identity, checkpoint.path.data()));
	CHECK(Status::Ok == f.model.ensureBound(0));
	RestoreId restore{};
	CHECK(Status::Ok == f.model.requestCheckpointRestore(0, checkpoint.identity, restore));
	CHECK(Status::Busy == f.model.serviceRestore(restore, 1));
	f.model.serviceArchive(4096); // Start the checkpoint reader.
	f.model.serviceArchive(4096); // Open the file and leave a foreground reader to drain.
	CHECK_FALSE(f.model.archive().checkpointReadIdle());
	CHECK(Status::Busy == f.model.requestSongReplacement(true));
	CHECK(Status::Busy == f.model.requestSongReplacement(true));
	for (unsigned i = 0; i < 1000 && !f.model.safeToRelease(); ++i) {
		f.model.serviceArchive(4096);
		f.model.serviceRetirement();
	}
	CHECK(f.model.safeToRelease());
	CHECK(Status::Ok == f.model.requestSongReplacement(true));
	CHECK_FALSE(f.model.engine().state(0).hasPhrase);
}
TEST(LoopLifecycle, viewExitStopsInitialTakeAndClosesOverdubImmediately) {
	Fixture f;
	CHECK(Status::Ok == f.model.ensureBound(0));
	CHECK(Status::Ok == f.model.dispatch({CommandType::Press, 0}));
	CHECK(Status::Ok == f.model.requestViewExit());
	CHECK(!f.model.engine().state(0).capture);
	CHECK(!f.model.engine().state(0).playing);
	f.take(0);
	CHECK(Status::Ok == f.model.dispatch({CommandType::Press, 0}));
	CHECK(f.model.engine().state(0).capture);
	CHECK(Status::Ok == f.model.requestViewExit());
	CHECK(!f.model.engine().state(0).capture);
	CHECK(f.model.engine().state(0).playing);
}
TEST(LoopLifecycle, replacementRequiresExplicitDiscardForVolatilePhrase) {
	Fixture f;
	f.take(0);
	CHECK(f.model.hasUnsavedAudio());
	CHECK(Status::Cancelled == f.model.requestSongReplacement(false));
	CHECK(f.host.registered);
	CHECK(Status::Busy == f.model.requestSongReplacement(true)); // Teardown is pending bounded retirement.
	CHECK(Status::Busy == f.model.requestSongReplacement(true)); // Repeated attempts must not bypass teardown.
	CHECK(!f.host.registered);
	for (unsigned i = 0; i < 1000 && !f.model.safeToRelease(); ++i) {
		f.model.serviceRetirement();
		f.model.serviceArchive(4096);
	}
	CHECK(f.model.safeToRelease());
	CHECK(Status::Ok == f.model.requestSongReplacement(true));
}
TEST(LoopLifecycle, discardConfirmationExpiresWhenAudioChangesBetweenPresses) {
	Fixture f;
	f.take(0);
	f.model.markAudioSaved(0, f.model.contentRevision(0));
	CHECK_FALSE(f.model.hasUnsavedAudio());
	CHECK(Status::Ok == f.model.dispatch({CommandType::Press, 0}));
	std::array<Generation, kTrackCount> confirmed{};
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		confirmed[slot] = f.model.contentRevision(slot);
	const auto captureMask = f.model.captureActivityMask();
	f.render(16);
	CHECK(Status::Ok == f.model.requestTransportStop());
	CHECK(f.model.hasUnsavedAudio());
	const auto replacement = f.model.requestSongReplacement(true, confirmed, captureMask);
	CHECK_EQUAL(int(Status::Cancelled), int(replacement));
	CHECK(f.host.registered);
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		confirmed[slot] = f.model.contentRevision(slot);
	CHECK(Status::Busy == f.model.requestSongReplacement(true, confirmed, f.model.captureActivityMask()));
}
TEST(LoopLifecycle, discardConfirmationExpiresWhenCaptureStartsWithoutAudioWrite) {
	Fixture f;
	f.take(0);
	CHECK(Status::Ok == f.model.requestTransportStop());
	f.model.markAudioSaved(0, f.model.contentRevision(0));
	CHECK(Status::Ok == f.model.dispatch({CommandType::Press, 0}));
	CHECK(Status::Ok == f.model.dispatch({CommandType::Release, 0}));
	CHECK(f.model.engine().state(0).playing);
	CHECK(Status::Ok == f.model.dispatch({CommandType::Press, 0}));
	const auto revision = f.model.contentRevision(0);
	const auto captureMask = f.model.captureActivityMask();
	CHECK_EQUAL(1, captureMask);
	CHECK_EQUAL(revision, f.model.contentRevision(0));
	std::array<Generation, kTrackCount> confirmed{};
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		confirmed[slot] = f.model.contentRevision(slot);
	const auto replacement = f.model.requestSongReplacement(true, confirmed, 0);
	CHECK_EQUAL(int(Status::Cancelled), int(replacement));
	CHECK(f.host.registered);
}
TEST(LoopLifecycle, captureActivityIncludesPendingInitialTakeStart) {
	Fixture f;
	CHECK(Status::Ok == f.model.ensureBound(0));
	auto settings = f.model.engine().settings();
	settings.startAction.mode = ActionMode::Synced;
	CHECK(Status::Ok == f.model.dispatch(Command::configure(settings)));
	CHECK(Status::Ok == f.model.dispatch({CommandType::Press, 0}));
	CHECK(f.model.engine().state(0).pending);
	CHECK_EQUAL(1, f.model.captureActivityMask());
	CHECK(f.model.hasUnsavedAudio());
	CHECK(Status::Cancelled == f.model.requestSongReplacement(false));
	CHECK(Status::Ok == f.model.requestTransportStop());
	CHECK_EQUAL(0, f.model.captureActivityMask());
}
TEST(LoopLifecycle, replacementDuringYieldingStartupInvalidatesAndDrainsTheStart) {
	Fixture f(false);
	Status replacement = Status::Ok;
	f.allocator.onAllocate = [&] {
		f.allocator.onAllocate = {};
		replacement = f.model.requestSongReplacement(true);
	};
	CHECK(Status::Stale == f.model.start());
	CHECK(Status::Busy == replacement);
	CHECK(f.model.safeToRelease());
	CHECK(Status::Ok == f.model.requestSongReplacement(true));
}
TEST(LoopLifecycle, viewExitDuringYieldingStartupInvalidatesButAllowsReopen) {
	Fixture f(false);
	Status exit = Status::Ok;
	f.allocator.onAllocate = [&] {
		f.allocator.onAllocate = {};
		exit = f.model.requestViewExit();
	};
	CHECK(Status::Stale == f.model.start());
	CHECK(Status::Busy == exit);
	CHECK_FALSE(f.host.registered);
	CHECK(Status::Ok == f.model.start());
	CHECK(f.model.ready());
}
TEST(LoopLifecycle, savedRevisionIsCleanAndLaterOverdubIsDirty) {
	Fixture f;
	f.take(0);
	const auto saved = f.model.contentRevision(0);
	f.model.markAudioSaved(0, saved);
	CHECK(!f.model.hasUnsavedAudio());
	CHECK(Status::Ok == f.model.dispatch({CommandType::Press, 0}));
	f.render(16);
	CHECK(Status::Ok == f.model.requestTransportStop());
	CHECK(f.model.contentRevision(0) != saved);
	CHECK(f.model.hasUnsavedAudio());
}
