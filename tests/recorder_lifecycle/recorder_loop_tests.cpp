#include "CppUTest/TestHarness.h"
#include "fixture.h"
#include "loop_station/audio_pipeline.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "storage/storage_manager.h"
#include <bit>
#include <memory>
#include <vector>

struct SampleRecorderLifecycleTestAccess {
	static Error initialize(SampleRecorder& r) { return r.initializeRecordingSample(); }
	static Error clean(SampleRecorder& r) { return r.cleanupAbortedRecording(); }
	static Error prepare(SampleRecorder& r) { return r.prepareLoopClusters(); }
	static Error copy(SampleRecorder& r) { return r.copyLoopRemainder(); }
	static void positions(SampleRecorder& r, uint32_t writer, uint32_t playback) {
		r.loopFrameCursor_ = writer;
		r.loopPlaybackCursor_ = playback;
	}
	static void pinFullCache(SampleRecorder& r) {
		for (uint32_t i = 0; i < 8; ++i) {
			if (!r.loopCache_[i].cluster) {
				auto* cluster = r.sample->clusters.getElement(i)->getCluster(r.sample, i, CLUSTER_DONT_LOAD);
				cluster->numReasonsHeldBySampleRecorder++;
				r.loopCache_[i] = {cluster, i, true, false};
			}
			r.loopCache_[i].dirty = true;
		}
	}
};
using namespace recorder_test;
TEST_GROUP(LoopRecorder){void setup() override{failure = Failure::NONE;
writeError = Error::NONE;
clusterLoadFails = false;
seekFails = closeFails = false;
onSeek = {};
onCreate = {};
onClose = {};
recycleReleasedClusters = false;
FatFS::loopFileBytes.clear();
}
void teardown() override {
	CHECK_EQUAL(0, liveAllocations);
	CHECK_EQUAL(0, liveSamples);
	CHECK_EQUAL(0, liveClusters);
	onSeek = {};
}
void initialize(SampleRecorder& r, uint32_t frames, Sample* source = nullptr, uint32_t sourceFrame = 0) {
	CHECK(SampleRecorderLifecycleTestAccess::initialize(r) == Error::NONE);
	r.numSamplesCaptured = 0;
	r.folderID = AudioRecordingFolder::CLIPS;
	r.audioFileNumber = 0;
	r.recordingNumChannels = r.sample->numChannels = 2;
	r.sample->byteDepth = 3;
	r.mode = AudioInputChannel::SPECIFIC_OUTPUT;
	r.captureOutputInputActive = true;
	CHECK(r.setupLoopOverdub(frames, source, 0, sourceFrame) == Error::NONE);
}
}
;

TEST(LoopRecorder, AddsEveryPassAndStopsInputWithoutChangingUntouchedFrames) {
	SampleRecorder r;
	initialize(r, 4);
	std::array<StereoSample, 4> dry;
	dry.fill({3200, 3200});
	std::array<StereoSample, 4> input;
	input.fill({2560, 5120});
	r.processLoopOverdub(dry, input);
	CHECK_EQUAL(4, r.numSamplesCaptured);
	CHECK_FALSE(r.loopOverdubReady());
	dry.fill({3200, 3200});
	r.processLoopOverdub(std::span(dry).first(2), std::span(input).first(2));
	CHECK_EQUAL(3520, dry[0].l);
	CHECK_EQUAL(3840, dry[0].r);
	r.stopLoopOverdubInput();
	CHECK_TRUE(r.loopOverdubReady());
	r.processLoopOverdub(dry, input);
	// Cursor was at frame 2. Those two frames have one addition, the next
	// two have two additions. Input supplied after stop must not be recorded.
	CHECK_EQUAL(3520, dry[0].l);
	CHECK_EQUAL(3520, dry[1].l);
	CHECK_EQUAL(3840, dry[2].l);
	CHECK_EQUAL(3840, dry[3].l);
}

TEST(LoopRecorder, EarlyStopCopiesOnlyTheUntouchedBackingRemainder) {
	SampleRecorder r;
	initialize(r, 4);
	std::array<StereoSample, 4> dry;
	dry.fill({3200, 3200});
	std::array<StereoSample, 4> input;
	input.fill({2560, 2560});
	r.processLoopOverdub(std::span(dry).first(1), std::span(input).first(1));
	r.stopLoopOverdubInput();
	CHECK_FALSE(r.loopOverdubReady());
	r.processLoopOverdub(std::span(dry).first(3), std::span(input).first(3));
	CHECK_TRUE(r.loopOverdubReady());
	r.processLoopOverdub(dry, input);
	CHECK_EQUAL(3520, dry[0].l);
	for (int i = 1; i < 4; ++i)
		CHECK_EQUAL(3200, dry[i].l);
}

TEST(LoopRecorder, ShortLoopWrapsRepeatedlyWithinOneAudioBuffer) {
	SampleRecorder r;
	initialize(r, 1);
	std::array<StereoSample, 8> dry;
	dry.fill({3200, 3200});
	std::array<StereoSample, 8> input;
	input.fill({2560, 2560});
	r.processLoopOverdub(dry, input);
	for (int i = 0; i < 8; ++i)
		CHECK_EQUAL(3200 + 320 * i, dry[i].l);
	CHECK_EQUAL(1, r.numSamplesCaptured);
	r.stopLoopOverdubInput();
	r.processLoopOverdub(std::span(dry).first(1), std::span(input).first(1));
	CHECK_EQUAL(5760, dry[0].l);
}

TEST(LoopRecorder, SdWriteSnapshotRetainsAudioChangesMadeDuringYield) {
	SampleRecorder r;
	initialize(r, 1);
	std::array<StereoSample, 1> dry{{{3200, 3200}}}, input{{{2560, 2560}}};
	r.processLoopOverdub(dry, input);
	r.file.emplace();
	r.filePathCreated.set("test.wav");
	onSeek = [&] { r.processLoopOverdub(dry, input); };
	CHECK(r.cardRoutine() == Error::NONE);
	onSeek = {};
	CHECK(r.cardRoutine() == Error::NONE);
	uint32_t value = 0;
	for (int i = 0; i < 3; ++i)
		value |= std::to_integer<uint32_t>(FatFS::loopFileBytes[44 + i]) << (8 * (i + 1));
	CHECK_EQUAL(30720, std::bit_cast<int32_t>(value));
}

TEST(LoopRecorder, WriteFailureIsLatchedAndTheWorkingSampleCanRollBack) {
	SampleRecorder r;
	initialize(r, 4);
	r.file.emplace();
	r.filePathCreated.set("test.wav");
	writeError = Error::SD_CARD;
	CHECK(r.cardRoutine() == Error::SD_CARD);
	CHECK(r.terminalError == Error::SD_CARD);
	CHECK_TRUE(r.hadCardError);
	CHECK(r.status == RecorderStatus::ABORTED);
	CHECK_FALSE(r.captureOutputInputActive);
	CHECK(r.storageFailure.stage == deluge::loop_station::StorageStage::WRITE);
	CHECK_EQUAL(FR_DISK_ERR, r.storageFailure.rawResult);
	r.abort();
	CHECK(r.cardRoutine() == Error::NONE);
}

TEST(LoopRecorder, BackgroundCopyCompletesRotatedBackingAfterPlaybackStops) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> dry{{{25600, 25600}, {51200, 51200}, {76800, 76800}, {102400, 102400}}};
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(dry, silence);
	SampleRecorder r;
	initialize(r, 4, source.sample, 2);
	// The rendered backing may contain transpose, envelope, or other playback
	// processing. Storage must use the retained WAV frame instead.
	std::array<StereoSample, 1> current{{{123, -456}}}, input{{{2560, 2560}}};
	r.processLoopOverdub(current, input);
	r.stopLoopOverdubInput();
	CHECK_FALSE(r.loopOverdubReady());
	r.file.emplace();
	r.filePathCreated.set("test.wav");
	// No more playback buffers are supplied.
	CHECK(r.cardRoutine() == Error::NONE);
	CHECK_TRUE(r.loopOverdubReady());
	const int32_t expected[] = {76800 * 8 + 2560, 102400 * 8, 25600 * 8, 51200 * 8};
	for (int frame = 0; frame < 4; ++frame) {
		uint32_t value = 0;
		for (int byte = 0; byte < 3; ++byte)
			value |= std::to_integer<uint32_t>(FatFS::loopFileBytes[44 + frame * 6 + byte]) << (8 * (byte + 1));
		CHECK_EQUAL(expected[frame], std::bit_cast<int32_t>(value));
	}
}

TEST(LoopRecorder, ProductionPipelineRepeatsAdditionsWithMonitoringOffAndKeepsStorageDry) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> dry;
	dry.fill({3200, -3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(dry, silence);
	SampleRecorder child;
	initialize(child, 4, source.sample);
	std::array<StereoSample, 4> input;
	input.fill({2560, -5120});
	int pitchCalls = 0;
	auto pitch = [&](std::span<StereoSample> audio) {
		++pitchCalls;
		// Distinct processing makes a reordered or duplicated recorder tap visible.
		for (auto& frame : audio) {
			frame.l *= 2;
			frame.r *= 2;
		}
	};
	// Envelope is half volume. Live monitoring is enabled on the first pass.
	deluge::loop_station::renderAudio(dry, input, &child, 1 << 25, 1 << 25, 1 << 24, true, pitch);
	CHECK_EQUAL(3840, dry[0].l);
	CHECK_EQUAL(-4480, dry[0].r);
	// An unrelated clip render still supplies the original backing. The recorder
	// must replace it with the first pass's additions, even with monitoring off.
	dry.fill({3200, -3200});
	deluge::loop_station::renderAudio(dry, silence, &child, 1 << 26, 1 << 26, 1 << 23, false, pitch);
	CHECK_EQUAL(3520, dry[0].l);
	CHECK_EQUAL(-3840, dry[0].r);
	child.stopLoopOverdubInput();
	dry.fill({3200, -3200});
	auto bypass = [](std::span<StereoSample>) {};
	deluge::loop_station::renderAudio(dry, input, &child, 1 << 26, 1 << 26, 1 << 24, false, bypass);
	CHECK_EQUAL(3520, dry[0].l);
	CHECK_EQUAL(-3840, dry[0].r);
	CHECK_EQUAL(2, pitchCalls);
}

TEST(LoopRecorder, SourcePrefetchIncludesSplitFramesAndLoopWrap) {
	for (uint32_t frames : {uint32_t(Cluster::size / 6), uint32_t(Cluster::size / 3)}) {
		SampleRecorder source;
		initialize(source, frames);
		std::vector<StereoSample> backing(frames, {3200, -3200});
		std::vector<StereoSample> silence(frames);
		source.processLoopOverdub(backing, silence);
		for (uint32_t start : {0u, frames - 12}) {
			SampleRecorder child;
			initialize(child, frames, source.sample, start);
			for (uint32_t cursor = 0; cursor < frames; cursor += 64) {
				CHECK(SampleRecorderLifecycleTestAccess::prepare(child) == Error::NONE);
				auto count = std::min<uint32_t>(64, frames - cursor);
				child.processLoopOverdub(std::span(backing).first(count), std::span(silence).first(count));
				CHECK_FALSE(child.captureOutputBackingMissing);
			}
			CHECK_EQUAL(frames, child.numSamplesCaptured);
		}
	}
}

TEST(LoopRecorder, ProductionPipelineKeepsEarlyOverdubAudibleWhileTailCopies) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> dry;
	dry.fill({3200, 3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(dry, silence);
	SampleRecorder child;
	initialize(child, 4, source.sample);
	std::array<StereoSample, 1> addition{{{2560, 2560}}};
	auto bypass = [](std::span<StereoSample>) {};
	deluge::loop_station::renderAudio(std::span(dry).first(1), addition, &child, 1 << 26, 1 << 26, 1 << 24, false,
	                                  bypass);
	child.stopLoopOverdubInput();
	CHECK_FALSE(child.loopOverdubReady());
	dry.fill({3200, 3200});
	deluge::loop_station::renderAudio(dry, silence, &child, 1 << 26, 1 << 26, 1 << 24, false, bypass);
	// Audible cursor wraps to the recorded frame before background copying runs.
	CHECK_EQUAL(3200, dry[0].l);
	CHECK_EQUAL(3520, dry[3].l);
	CHECK_FALSE(child.loopOverdubReady());
	CHECK(SampleRecorderLifecycleTestAccess::copy(child) == Error::NONE);
	CHECK_TRUE(child.loopOverdubReady());
}

TEST(LoopRecorder, ProductionPipelineFadesBackingWithoutMutingMonitorOrRecordingFade) {
	SampleRecorder child;
	initialize(child, 4);
	std::array<StereoSample, 4> dry;
	dry.fill({3200, 3200});
	std::array<StereoSample, 4> input;
	input.fill({2560, 2560});
	auto bypass = [](std::span<StereoSample>) {};
	deluge::loop_station::renderAudio(dry, input, &child, 1 << 26, 0, 1 << 24, true, bypass);
	CHECK_EQUAL(2720, dry[0].l);
	CHECK_EQUAL(320, dry[3].l);
	child.stopLoopOverdubInput();
	dry.fill({3200, 3200});
	deluge::loop_station::renderAudio(dry, input, &child, 1 << 26, 1 << 26, 1 << 24, false, bypass);
	for (auto frame : dry)
		CHECK_EQUAL(3520, frame.l);
}

TEST(LoopRecorder, AbortDuringSeekDoesNotWriteOrReleaseSnapshotInFlight) {
	SampleRecorder r;
	initialize(r, 4);
	r.file.emplace();
	r.filePathCreated.set("test.wav");
	onSeek = [&] {
		r.abort();
		CHECK(r.cardRoutine() == Error::NONE);
	};
	CHECK(r.cardRoutine() == Error::NONE);
	CHECK_EQUAL(0, FatFS::loopFileBytes.size());
	onSeek = {};
	CHECK(r.cardRoutine() == Error::NONE);
}

TEST(LoopRecorder, StoppedInputKeepsPartialAdditionAudibleWhileTailCopies) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> dry;
	dry.fill({3200, 3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(dry, silence);
	SampleRecorder r;
	initialize(r, 4, source.sample);
	std::array<StereoSample, 4> input;
	input.fill({2560, 2560});
	r.processLoopOverdub(std::span(dry).first(1), std::span(input).first(1));
	r.stopLoopOverdubInput();
	// Playback runs over the untouched three frames and wraps to the addition.
	r.processLoopOverdub(dry, input);
	CHECK_EQUAL(3520, dry[3].l);
	CHECK_EQUAL(1, r.numSamplesCaptured);
	r.file.emplace();
	r.filePathCreated.set("test.wav");
	CHECK(r.cardRoutine() == Error::NONE);
	CHECK(r.loopOverdubReady());
	dry.fill({3200, 3200});
	r.processLoopOverdub(dry, input);
	CHECK_EQUAL(3520, dry[3].l);
}

TEST(LoopRecorder, ParentCleanupDefersUntilAbortedDependentReleasesSource) {
	SampleRecorder parent;
	CHECK(SampleRecorderLifecycleTestAccess::initialize(parent) == Error::NONE);
	parent.retainForCommit();
	parent.filePathCreated.set("TEMP/parent.wav");
	parent.folderID = AudioRecordingFolder::CLIPS;
	parent.audioFileNumber = 0;
	SampleRecorder child;
	initialize(child, 4, parent.sample);
	Sample* sample = parent.sample;
	parent.releaseCommit(false);
	child.abort();
	int oldUnlinks = unlinks;
	for (int i = 0; i < 3; ++i) {
		CHECK(parent.cardRoutine() == Error::NONE);
		POINTERS_EQUAL(sample, parent.sample);
		CHECK_EQUAL(2, sample->numReasonsToBeLoaded);
		CHECK(parent.status == RecorderStatus::ABORTED);
		CHECK_EQUAL(oldUnlinks, unlinks);
	}
	CHECK(child.cardRoutine() == Error::NONE);
	CHECK_EQUAL(1, sample->numReasonsToBeLoaded);
	CHECK(parent.cardRoutine() == Error::NONE);
	POINTERS_EQUAL(nullptr, parent.sample);
	CHECK_EQUAL(oldUnlinks + 1, unlinks);
	CHECK(parent.status == RecorderStatus::AWAITING_DELETION);
	CHECK(parent.cardRoutine() == Error::NONE);
}

TEST(LoopRecorder, EightDependentsDrainWithoutPrematureParentDeletion) {
	SampleRecorder parent;
	initialize(parent, 4);
	parent.retainForCommit();
	std::array<SampleRecorder, 8> children;
	for (auto& child : children)
		initialize(child, 4, parent.sample);
	parent.releaseCommit(false);
	for (auto& child : children) {
		CHECK(parent.cardRoutine() == Error::NONE);
		CHECK(parent.sample != nullptr);
		child.abort();
		CHECK(child.cardRoutine() == Error::NONE);
	}
	CHECK(parent.cardRoutine() == Error::NONE);
	POINTERS_EQUAL(nullptr, parent.sample);
}

TEST(LoopRecorder, ChildFirstAndRepeatedCleanupReleaseReasonsOnce) {
	SampleRecorder parent;
	initialize(parent, 4);
	SampleRecorder child;
	initialize(child, 4, parent.sample);
	parent.abort();
	child.abort();
	CHECK(child.cardRoutine() == Error::NONE);
	CHECK(child.cardRoutine() == Error::NONE);
	CHECK_EQUAL(1, parent.sample->numReasonsToBeLoaded);
	CHECK(parent.cardRoutine() == Error::NONE);
	CHECK(parent.cardRoutine() == Error::NONE);
}

TEST(LoopRecorder, CleanupCloseCanReenterWithoutDoubleDetach) {
	SampleRecorder parent;
	initialize(parent, 4);
	SampleRecorder child;
	initialize(child, 4, parent.sample);
	parent.file.emplace();
	parent.abort();
	child.abort();
	onClose = [&] {
		CHECK(parent.cardRoutine() == Error::NONE);
		CHECK(parent.sample != nullptr);
		CHECK(child.cardRoutine() == Error::NONE);
	};
	CHECK(parent.cardRoutine() == Error::NONE);
	onClose = {};
	POINTERS_EQUAL(nullptr, parent.sample);
}

TEST(LoopRecorder, WriterDoesNotStarveLaterDirtyClusterDuringRepeatedEarlyUpdates) {
	SampleRecorder r;
	uint32_t frames = Cluster::size / 6 + 16;
	initialize(r, frames);
	std::vector<StereoSample> dry(frames, {3200, 3200}), input(frames, {2560, 2560});
	r.processLoopOverdub(dry, input);
	r.file.emplace();
	r.filePathCreated.set("test.wav");
	onSeek = [&] { r.processLoopOverdub(std::span(dry).first(1), std::span(input).first(1)); };
	CHECK(r.cardRoutine() == Error::NONE);
	CHECK(r.cardRoutine() == Error::NONE);
	onSeek = {};
	CHECK(filePosition > Cluster::size);
}

TEST(LoopRecorder, MissingPrefetchReportsBackingUnderrunWithoutInventingSdFailure) {
	SampleRecorder r;
	initialize(r, 4);
	r.sample->clusters.getElement(0)->cluster->loaded = false;
	std::array<StereoSample, 1> dry{{{3200, 3200}}}, input{{{2560, 2560}}};
	r.processLoopOverdub(dry, input);
	CHECK(r.captureOutputBackingMissing);
	CHECK(r.terminalError == Error::NONE);
	CHECK_EQUAL(0, r.numSamplesCaptured);
	r.abort();
	CHECK(r.cardRoutine() == Error::NONE);
}

#include "loop_station/commit.h"
TEST(LoopRecorder, RealCommitCoordinatorHoldsParentThroughYieldingRollback) {
	SampleRecorder parent;
	initialize(parent, 4);
	parent.terminalError = Error::SD_CARD_FULL;
	parent.abort();
	parent.retainForCommit();
	SampleRecorder child;
	initialize(child, 4, parent.sample);
	Sample* failed = parent.sample;
	failed->addReason(); // clip
	failed->addReason(); // staged history
	bool busy = false, detached = false;
	int restored = 0, notified = 0;
	deluge::loop_station::resolveCaptureCommit(
	    busy, parent, false, [] { return true; }, [&] { return &child; },
	    [&] {
		    CHECK(busy);
		    CHECK_FALSE(child.captureOutputInputActive);
		    CHECK(child.status == RecorderStatus::ABORTED);
		    detached = true;
	    },
	    [&] {
		    CHECK(detached);
		    CHECK(busy);
		    CHECK(parent.isCommitPending());
		    CHECK(parent.cardRoutine() == Error::NONE);
		    CHECK(child.cardRoutine() == Error::NONE);
		    // Reentrant completion must not publish or notify a second time.
		    deluge::loop_station::resolveCaptureCommit(
		        busy, parent, true,
		        [&] {
			        ++restored;
			        return true;
		        },
		        [] { return static_cast<SampleRecorder*>(nullptr); }, [] {}, [] {}, [&](bool) { ++notified; });
		    failed->removeReason("clip rollback");
		    failed->removeReason("history rollback");
		    ++restored;
	    },
	    [&](bool success) {
		    CHECK_FALSE(success);
		    CHECK(parent.isCommitPending());
		    CHECK(busy);
		    CHECK(parent.terminalError == Error::SD_CARD_FULL);
		    ++notified;
	    });
	CHECK_FALSE(busy);
	CHECK_EQUAL(1, restored);
	CHECK_EQUAL(1, notified);
	CHECK(parent.cardRoutine() == Error::NONE);
	POINTERS_EQUAL(nullptr, parent.sample);
}

TEST(LoopRecorder, SuccessfulCommitLeavesImmediateDependentRunning) {
	SampleRecorder parent;
	initialize(parent, 4);
	parent.retainForCommit();
	SampleRecorder child;
	initialize(child, 4, parent.sample);
	bool busy = false, published = false;
	deluge::loop_station::resolveCaptureCommit(
	    busy, parent, true,
	    [&] {
		    published = true;
		    return true;
	    },
	    [&] { return &child; }, [] { FAIL("Quiesced successful capture"); },
	    [] { FAIL("Rolled back successful capture"); },
	    [&](bool success) {
		    CHECK(success);
		    CHECK(published);
		    CHECK(busy);
	    });
	CHECK(child.captureOutputInputActive);
	CHECK(child.status == RecorderStatus::CAPTURING_DATA);
	CHECK(parent.status == RecorderStatus::AWAITING_DELETION);
}

TEST(LoopRecorder, PublicationFailureFindsDependentCreatedDuringYield) {
	SampleRecorder parent;
	initialize(parent, 4);
	parent.retainForCommit();
	SampleRecorder child;
	SampleRecorder* dependent = nullptr;
	bool busy = false;
	int rolledBack = 0;
	deluge::loop_station::resolveCaptureCommit(
	    busy, parent, true,
	    [&] {
		    initialize(child, 4, parent.sample);
		    dependent = &child;
		    return false;
	    },
	    [&] { return dependent; },
	    [&] {
		    CHECK(child.status == RecorderStatus::ABORTED);
		    CHECK_FALSE(child.captureOutputInputActive);
	    },
	    [&] { ++rolledBack; },
	    [&](bool success) {
		    CHECK_FALSE(success);
		    CHECK(parent.isCommitPending());
	    });
	CHECK_EQUAL(1, rolledBack);
	CHECK_FALSE(busy);
	CHECK(parent.cardRoutine() == Error::NONE);
	CHECK(parent.sample != nullptr);
	CHECK(child.cardRoutine() == Error::NONE);
	CHECK(parent.cardRoutine() == Error::NONE);
	POINTERS_EQUAL(nullptr, parent.sample);
}

TEST(LoopRecorder, SeekFailureAbortsBeforePublishingClusterAddress) {
	SampleRecorder r;
	initialize(r, 4);
	r.file.emplace();
	r.filePathCreated.set("test.wav");
	seekFails = true;
	CHECK(r.cardRoutine() == Error::SD_CARD);
	CHECK(r.status == RecorderStatus::ABORTED);
	CHECK(r.storageFailure.stage == deluge::loop_station::StorageStage::SEEK);
	CHECK_EQUAL(0, r.sample->clusters.getElement(0)->sdAddress);
	seekFails = false;
	CHECK(r.cardRoutine() == Error::NONE);
}

TEST(LoopRecorder, CloseFailureRetainsItsStageThroughCleanup) {
	SampleRecorder r;
	initialize(r, 4);
	r.file.emplace();
	r.filePathCreated.set("test.wav");
	r.status = RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING;
	CHECK(r.cardRoutine() == Error::NONE); // Flush dirty first cluster.
	closeFails = true;
	CHECK(r.cardRoutine() == Error::SD_CARD);
	CHECK(r.storageFailure.stage == deluge::loop_station::StorageStage::CLOSE);
	CHECK(r.status == RecorderStatus::ABORTED);
	closeFails = false;
	CHECK(r.cardRoutine() == Error::NONE);
	CHECK_EQUAL(FR_DISK_ERR, r.storageFailure.rawResult);
}

TEST(LoopRecorder, FinishingPrefetchFollowsPlaybackAfterWriterStops) {
	uint32_t frames = Cluster::size * 12 / 6;
	SampleRecorder source;
	initialize(source, frames);
	SampleRecorder r;
	initialize(r, frames, source.sample);
	r.numSamplesCaptured = frames;
	r.stopLoopOverdubInput();
	SampleRecorderLifecycleTestAccess::positions(r, 0, Cluster::size * 6 / 6);
	CHECK(SampleRecorderLifecycleTestAccess::prepare(r) == Error::NONE);
	std::array<StereoSample, 1> dry{{{3200, 3200}}};
	r.processLoopOverdub(dry, {});
	CHECK_FALSE(r.captureOutputBackingMissing);
	CHECK_EQUAL(frames, r.numSamplesCaptured);
}

TEST(LoopRecorder, BackgroundCopyWaitsForPinnedCacheThenMakesProgress) {
	uint32_t frames = Cluster::size * 12 / 6;
	SampleRecorder source;
	initialize(source, frames);
	SampleRecorder r;
	initialize(r, frames, source.sample);
	SampleRecorderLifecycleTestAccess::pinFullCache(r);
	uint32_t copied = Cluster::size * 8 / 6;
	r.numSamplesCaptured = copied;
	r.stopLoopOverdubInput();
	SampleRecorderLifecycleTestAccess::positions(r, copied, 0);
	CHECK(SampleRecorderLifecycleTestAccess::copy(r) == Error::NONE);
	CHECK_FALSE(r.captureOutputBackingMissing);
	CHECK_EQUAL(copied, r.numSamplesCaptured);
	r.file.emplace();
	r.filePathCreated.set("test.wav");
	for (int i = 0; i < 20 && r.numSamplesCaptured == copied; ++i)
		CHECK(r.cardRoutine() == Error::NONE);
	CHECK(r.numSamplesCaptured > copied);
	CHECK_FALSE(r.captureOutputBackingMissing);
}

TEST(LoopRecorder, OneHundredSuccessiveOverdubsReleaseParentsAndAccumulateAudio) {
	std::array<Sample*, 100> retired{};
	auto parent = std::make_unique<SampleRecorder>();
	initialize(*parent, 4);
	std::array<StereoSample, 4> dry;
	dry.fill({3200, 3200});
	std::array<StereoSample, 4> input;
	input.fill({2560, 2560});
	std::array<StereoSample, 4> silence{};
	// Production overdubs always retain an existing populated WAV. Seed the
	// fixture parent so each child reads its backing from stored PCM.
	parent->processLoopOverdub(dry, silence);
	parent->stopLoopOverdubInput();
	for (int i = 0; i < 100; ++i) {
		parent->retainForCommit();
		auto child = std::make_unique<SampleRecorder>();
		initialize(*child, 4, parent->sample);
		auto* backing = parent->sample;
		retired[i] = backing;
		CHECK_EQUAL(2, backing->numReasonsToBeLoaded);
		child->processLoopOverdub(dry, input);
		child->stopLoopOverdubInput();
		CHECK(child->loopOverdubReady());
		// Preview during delayed publication must not add the supplied input again.
		for (int n = 0; n < 5; ++n)
			child->processLoopOverdub(dry, input);
		CHECK_EQUAL(3200 + (i + 1) * 320, dry[0].l);
		CHECK_FALSE(child->captureOutputBackingMissing);
		parent->releaseCommit(true);
		parent.reset();
		CHECK_EQUAL(1, backing->numReasonsToBeLoaded);
		parent = std::move(child);
	}
	parent.reset();
	// The fixture has no AudioFileManager eviction pass. Reclaim Samples whose
	// last source reason drained after their recorder was already destroyed.
	for (auto* sample : retired) {
		CHECK_EQUAL(0, sample->numReasonsToBeLoaded);
		sample->~Sample();
		delugeDealloc(sample);
	}
}

TEST(LoopRecorder, BackgroundCopyWaitsForAsynchronousSourceRead) {
	SampleRecorder parent;
	initialize(parent, 4);
	SampleRecorder child;
	initialize(child, 4, parent.sample);
	child.stopLoopOverdubInput();
	auto* backing = parent.sample->clusters.getElement(0)->cluster;
	backing->loaded = false;
	CHECK(SampleRecorderLifecycleTestAccess::copy(child) == Error::NONE);
	CHECK_EQUAL(0, child.numSamplesCaptured);
	CHECK_FALSE(child.captureOutputBackingMissing);
	backing->loaded = true;
	CHECK(SampleRecorderLifecycleTestAccess::copy(child) == Error::NONE);
	CHECK(child.loopOverdubReady());
	CHECK_FALSE(child.captureOutputBackingMissing);
}
