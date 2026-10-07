#include "CppUTest/TestHarness.h"
#include "fixture.h"
#include "loop_station/audio_pipeline.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "storage/storage_manager.h"
#include <algorithm>
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
	static int sourceStatus(SampleRecorder& r, uint32_t frame) {
		StereoSample sample{};
		return static_cast<int>(r.readLoopSourceFrame(frame, sample));
	}
	static int sourceReady() { return static_cast<int>(SampleRecorder::LoopReadResult::READY); }
	static int sourcePending() { return static_cast<int>(SampleRecorder::LoopReadResult::PENDING); }
	static uint32_t pendingCount(SampleRecorder& r) { return r.loopPendingCount_; }
	static uint64_t acceptedFrames(SampleRecorder& r) { return r.loopAcceptedFrames_; }
	static uint64_t appliedFrames(SampleRecorder& r) { return r.loopAppliedFrames_; }
	static bool sourceCached(SampleRecorder& r, Sample* sample, uint32_t clusterIndex) {
		return std::any_of(r.loopSourceCache_.begin(), r.loopSourceCache_.end(), [&](auto const& entry) {
			return entry.cluster && entry.sample == sample && entry.index == clusterIndex;
		});
	}
	static uint32_t sourceCacheSize(SampleRecorder& r) {
		return std::count_if(r.loopSourceCache_.begin(), r.loopSourceCache_.end(),
		                     [](auto const& entry) { return entry.cluster != nullptr; });
	}
	static bool destinationCached(SampleRecorder& r, uint32_t index) { return r.loopCacheAt(index) != nullptr; }
	static bool destinationDirty(SampleRecorder& r, uint32_t index) {
		auto* entry = r.loopCacheAt(index);
		return entry && entry->dirty;
	}
	static bool destinationWriting(SampleRecorder& r, uint32_t index) {
		auto* entry = r.loopCacheAt(index);
		return entry && entry->writing;
	}
	static void releaseLoopCache(SampleRecorder& r) { r.releaseLoopClusters(); }
	static bool captureBufferAllocated(SampleRecorder& r) { return r.loopPendingFrames_ != nullptr; }
	static bool writeSnapshotAllocated(SampleRecorder& r) { return r.loopWriteSnapshot_ != nullptr; }
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
asyncClusterLoads = false;
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
	asyncClusterLoads = false;
}
void initialize(SampleRecorder& r, uint32_t frames, Sample* source = nullptr, uint32_t sourceFrame = 0,
                SampleRecorder* sourceRecorder = nullptr) {
	CHECK(SampleRecorderLifecycleTestAccess::initialize(r) == Error::NONE);
	r.numSamplesCaptured = 0;
	r.folderID = AudioRecordingFolder::CLIPS;
	r.audioFileNumber = 0;
	r.recordingNumChannels = r.sample->numChannels = 2;
	r.sample->byteDepth = 3;
	r.mode = AudioInputChannel::SPECIFIC_OUTPUT;
	r.captureOutputInputActive = true;
	CHECK(r.setupLoopOverdub(frames, source, 0, sourceFrame, sourceRecorder) == Error::NONE);
}
}
;

TEST(LoopRecorder, NextGenerationCapturesBeforeParentTailOrFileIsWritten) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> dry;
	dry.fill({3200, 3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(dry, silence);
	SampleRecorder first;
	initialize(first, 4, source.sample);
	std::array<StereoSample, 1> firstInput{{{2560, 2560}}};
	first.processLoopOverdub(std::span(dry).first(1), firstInput);
	first.sealLoopOverdub();
	CHECK_EQUAL(1, first.numSamplesCaptured);
	CHECK_FALSE(first.loopOverdubReady());
	CHECK_FALSE(first.file.has_value());
	SampleRecorder second;
	initialize(second, 4, first.sample, 1, &first);
	std::array<StereoSample, 4> secondInput;
	secondInput.fill({5120, 5120});
	auto bypass = [](std::span<StereoSample>) {};
	deluge::loop_station::renderAudio(dry, secondInput, &second, 1 << 26, 1 << 26, 1 << 24, false, bypass, false);
	CHECK_EQUAL(4, second.numSamplesCaptured);
	CHECK_EQUAL(1, first.numSamplesCaptured);
	CHECK_EQUAL(3200, dry[0].l);
	CHECK_EQUAL(3520, dry[3].l);
	second.sealLoopOverdub();
	deluge::loop_station::renderAudio(dry, silence, &second, 1 << 26, 1 << 26, 1 << 24, false, bypass, false);
	CHECK_EQUAL(3840, dry[0].l);
	CHECK_EQUAL(4160, dry[3].l);
	CHECK_FALSE(second.captureOutputBackingMissing);
	CHECK_FALSE(first.file.has_value());
}

TEST(LoopRecorder, CompletedGenerationStaysAliveUntilItsPlaybackAndDependentReadersReleaseIt) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(silence, silence);
	source.retainForCommit();
	source.retainLoopReader();
	source.status = RecorderStatus::COMPLETE;
	{
		SampleRecorder child;
		initialize(child, 4, source.sample, 0, &source);
		source.releaseCommit(true);
		CHECK(source.status == RecorderStatus::COMPLETE);
		CHECK_TRUE(source.pointerHeldElsewhere);
		source.releaseLoopReader();
		CHECK(source.status == RecorderStatus::COMPLETE);
	}
	CHECK(source.status == RecorderStatus::AWAITING_DELETION);
	CHECK_FALSE(source.pointerHeldElsewhere);
}

TEST(LoopRecorder, DurablePlaybackProviderWaitsForReplacementVoiceBeforeReleasingReader) {
	SampleRecorder provider;
	initialize(provider, 4);
	provider.retainForCommit();
	provider.retainLoopReader(); // AudioClip remains on this provider until its WAV voice is ready.
	provider.status = RecorderStatus::COMPLETE;
	provider.releaseCommit(true);
	CHECK(provider.status == RecorderStatus::COMPLETE);
	CHECK_TRUE(provider.hasLoopReaders());

	bool replacementVoiceReady = false;
	if (replacementVoiceReady)
		provider.releaseLoopReader();
	CHECK_TRUE(provider.hasLoopReaders());
	CHECK(provider.status == RecorderStatus::COMPLETE);

	replacementVoiceReady = true;
	if (replacementVoiceReady)
		provider.releaseLoopReader();
	CHECK_FALSE(provider.hasLoopReaders());
	CHECK(provider.status == RecorderStatus::AWAITING_DELETION);
}

TEST(LoopRecorder, AbortingAutomaticDubWithLinearParentReleasesItsSourceReader) {
	SampleRecorder linearParent;
	CHECK(SampleRecorderLifecycleTestAccess::initialize(linearParent) == Error::NONE);
	linearParent.folderID = AudioRecordingFolder::CLIPS;
	linearParent.recordingNumChannels = linearParent.sample->numChannels = 2;
	linearParent.sample->byteDepth = 3;

	SampleRecorder automaticDub;
	initialize(automaticDub, 4, linearParent.sample, 3, &linearParent);
	POINTERS_EQUAL(&linearParent, automaticDub.loopSourceRecorder());
	CHECK_FALSE(linearParent.isLoopOverdub());
	CHECK_TRUE(linearParent.hasLoopReaders());

	automaticDub.abort();
	CHECK(automaticDub.cardRoutine() == Error::NONE);
	CHECK(linearParent.status == RecorderStatus::CAPTURING_DATA);
	CHECK_FALSE(linearParent.hasLoopReaders());

	linearParent.abort();
	CHECK(linearParent.cardRoutine() == Error::NONE);
}

TEST(LoopRecorder, SixteenShortGenerationsRemainAudibleWithoutMaterializingAnyParentTail) {
	SampleRecorder source;
	initialize(source, 256);
	std::array<StereoSample, 256> dry;
	dry.fill({3200, 3200});
	std::array<StereoSample, 256> silence{};
	source.processLoopOverdub(dry, silence);
	std::array<SampleRecorder, 16> takes;
	std::array<StereoSample, 1> input{{{2560, 2560}}};
	for (size_t i = 0; i < takes.size(); ++i) {
		auto* parent = i ? &takes[i - 1] : nullptr;
		initialize(takes[i], 256, parent ? parent->sample : source.sample, i ? 1 : 0, parent);
		takes[i].processLoopOverdub(std::span(dry).first(1), input);
		takes[i].sealLoopOverdub();
		CHECK_EQUAL(1, takes[i].numSamplesCaptured);
		CHECK_FALSE(takes[i].captureOutputBackingMissing);
	}
	auto& last = takes.back();
	last.processLoopOverdub(dry, silence);
	uint64_t sum = 0;
	for (auto frame : dry)
		sum += frame.l;
	CHECK_EQUAL(256 * 3200 + 16 * 320, sum);
	CHECK_FALSE(last.captureOutputBackingMissing);
	for (auto const& take : takes) {
		CHECK_EQUAL(1, take.numSamplesCaptured);
		CHECK_FALSE(take.file.has_value());
	}
}

TEST(LoopRecorder, EightTracksStartSecondTakesWhileEveryFirstTakeIsStillUnsaved) {
	std::array<SampleRecorder, 8> sources, first, second;
	std::array<StereoSample, 8> dry, silence{};
	std::array<StereoSample, 1> firstInput{{{2560, 2560}}};
	std::array<StereoSample, 8> secondInput;
	secondInput.fill({5120, 5120});
	for (size_t i = 0; i < sources.size(); ++i) {
		initialize(sources[i], 8);
		dry.fill({3200, 3200});
		sources[i].processLoopOverdub(dry, silence);
		initialize(first[i], 8, sources[i].sample);
		first[i].processLoopOverdub(std::span(dry).first(1), firstInput);
		first[i].sealLoopOverdub();
		initialize(second[i], 8, first[i].sample, 1, &first[i]);
		second[i].processLoopOverdub(dry, secondInput);
		CHECK_EQUAL(8, second[i].numSamplesCaptured);
		CHECK_EQUAL(1, first[i].numSamplesCaptured);
		CHECK_FALSE(first[i].file.has_value());
	}
	for (auto& recorder : second) {
		recorder.sealLoopOverdub();
		recorder.processLoopOverdub(dry, silence);
		CHECK_EQUAL(3840, dry[0].l);
		CHECK_EQUAL(4160, dry[7].l);
		CHECK_FALSE(recorder.captureOutputBackingMissing);
	}
}

TEST(LoopRecorder, AbortedParentCannotBeCleanedWhileAnUnpublishedGenerationReadsIt) {
	SampleRecorder parent;
	initialize(parent, 4);
	std::array<StereoSample, 4> dry{}, input{};
	parent.processLoopOverdub(dry, input);
	parent.retainForCommit();
	SampleRecorder child;
	initialize(child, 4, parent.sample, 0, &parent);
	parent.releaseCommit(false);
	CHECK(SampleRecorderLifecycleTestAccess::clean(parent) == Error::NONE);
	CHECK(parent.sample != nullptr);
	CHECK(parent.hasLoopReaders());
	child.abort();
	CHECK(SampleRecorderLifecycleTestAccess::clean(child) == Error::NONE);
	CHECK_FALSE(parent.hasLoopReaders());
	CHECK(SampleRecorderLifecycleTestAccess::clean(parent) == Error::NONE);
	POINTERS_EQUAL(nullptr, parent.sample);
}

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

TEST(LoopRecorder, PendingSourceRetainsInputUntilRawAudioIsReady) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> sourceBacking;
	sourceBacking.fill({3200, -3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(sourceBacking, silence);

	SampleRecorder child;
	initialize(child, 4, source.sample);
	auto* sourceCluster = source.sample->clusters.getElement(0)->cluster;
	sourceCluster->loaded = false;

	std::array<StereoSample, 4> renderedBacking;
	// Capture-side work may wait without turning an asynchronous source load
	// into corrupt backing or dropping already accepted input.
	renderedBacking.fill({123456, -654321});
	std::array<StereoSample, 4> input;
	input.fill({2560, 5120});
	child.processLoopOverdub(renderedBacking, input);

	CHECK_EQUAL(0, child.numSamplesCaptured);
	CHECK_FALSE(child.captureOutputBackingMissing);
	CHECK_EQUAL(123456, renderedBacking[0].l);
	CHECK_EQUAL(-654321, renderedBacking[0].r);
	// Closing the input gate must not discard samples already accepted.
	child.stopLoopOverdubInput();
	CHECK_FALSE(child.loopOverdubReady());
	sourceCluster->loaded = true;
	child.processLoopOverdub(renderedBacking, silence);
	CHECK_EQUAL(4, child.numSamplesCaptured);
	CHECK_TRUE(child.loopOverdubReady());
	CHECK_FALSE(child.captureOutputBackingMissing);
	for (auto frame : renderedBacking) {
		CHECK_EQUAL(3520, frame.l);
		CHECK_EQUAL(-2560, frame.r);
	}
}

TEST(LoopRecorder, SealedPendingInputReleasesItsFifoAfterDrain) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> backing;
	backing.fill({3200, 3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(backing, silence);
	SampleRecorder child;
	initialize(child, 4, source.sample);
	auto* sourceCluster = source.sample->clusters.getElement(0)->cluster;
	sourceCluster->loaded = false;
	std::array<StereoSample, 1> input{{{2560, 0}}};
	child.processLoopOverdub(std::span(backing).first(1), input);
	CHECK(SampleRecorderLifecycleTestAccess::captureBufferAllocated(child));
	CHECK_EQUAL(1, SampleRecorderLifecycleTestAccess::pendingCount(child));
	child.sealLoopOverdub();
	sourceCluster->loaded = true;
	child.file.emplace();
	child.filePathCreated.set("test.wav");
	for (int i = 0; i < 8 && child.status != RecorderStatus::COMPLETE; ++i)
		CHECK(child.cardRoutine() == Error::NONE);
	CHECK(child.status == RecorderStatus::COMPLETE);
	CHECK_FALSE(SampleRecorderLifecycleTestAccess::captureBufferAllocated(child));
	CHECK_FALSE(SampleRecorderLifecycleTestAccess::writeSnapshotAllocated(child));
}

TEST(LoopRecorder, SuccessfulCloseReleasesWriteSnapshotAfterCloseReturns) {
	SampleRecorder recorder;
	initialize(recorder, 1);
	std::array<StereoSample, 1> backing{{{3200, 3200}}}, silence{};
	recorder.processLoopOverdub(backing, silence);
	recorder.sealLoopOverdub();
	recorder.file.emplace();
	recorder.filePathCreated.set("test.wav");
	CHECK(SampleRecorderLifecycleTestAccess::writeSnapshotAllocated(recorder));
	onClose = [&] { CHECK(SampleRecorderLifecycleTestAccess::writeSnapshotAllocated(recorder)); };
	for (int i = 0; i < 4 && recorder.status != RecorderStatus::COMPLETE; ++i)
		CHECK(recorder.cardRoutine() == Error::NONE);
	onClose = {};
	CHECK(recorder.status == RecorderStatus::COMPLETE);
	CHECK_FALSE(SampleRecorderLifecycleTestAccess::writeSnapshotAllocated(recorder));
}

TEST(LoopRecorder, MaterializedGenerationReleasesAncestryWithDependentReaders) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> backing;
	backing.fill({3200, 3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(backing, silence);
	SampleRecorder child;
	initialize(child, 4, source.sample, 0, &source);
	child.processLoopOverdub(backing, silence);
	SampleRecorder grandchild;
	initialize(grandchild, 4, child.sample, 0, &child);
	CHECK(child.hasLoopReaders());
	child.file.emplace();
	child.filePathCreated.set("child.wav");
	CHECK(child.cardRoutine() == Error::NONE);
	POINTERS_EQUAL(nullptr, child.loopSourceRecorder());
	CHECK(child.hasLoopSource()); // Persistent raw-source mode survives ancestry release.
	CHECK(child.hasLoopReaders());
	CHECK_EQUAL(SampleRecorderLifecycleTestAccess::sourceReady(),
	            SampleRecorderLifecycleTestAccess::sourceStatus(grandchild, 2));
}

TEST(LoopRecorder, OneHundredParentChildHandoffsReleaseTemporaryBuffersAtEachMilestone) {
	std::array<SampleRecorder, 100> generations;
	std::array<StereoSample, 4> backing;
	backing.fill({3200, 3200});
	std::array<StereoSample, 4> silence{};
	std::array<StereoSample, 4> input;
	input.fill({2560, -1280});
	for (size_t i = 0; i < generations.size(); ++i) {
		auto* sourceRecorder = i ? &generations[i - 1] : nullptr;
		initialize(generations[i], 4, sourceRecorder ? sourceRecorder->sample : nullptr, 0, sourceRecorder);
		generations[i].processLoopOverdub(backing, sourceRecorder ? std::span<const StereoSample>(input)
		                                                          : std::span<const StereoSample>(silence));
		CHECK_EQUAL(4, generations[i].numSamplesCaptured);
		generations[i].sealLoopOverdub();
		generations[i].file.emplace();
		generations[i].filePathCreated.set("generation.wav");
		for (int attempt = 0; attempt < 8 && generations[i].status != RecorderStatus::COMPLETE; ++attempt)
			CHECK(generations[i].cardRoutine() == Error::NONE);
		CHECK(generations[i].status == RecorderStatus::COMPLETE);
		CHECK_FALSE(SampleRecorderLifecycleTestAccess::captureBufferAllocated(generations[i]));
		CHECK_FALSE(SampleRecorderLifecycleTestAccess::writeSnapshotAllocated(generations[i]));
		if (i) {
			POINTERS_EQUAL(nullptr, generations[i].loopSourceRecorder());
			CHECK(generations[i].hasLoopSource());
		}
		CHECK_EQUAL(i + 1, liveAllocations); // Only durable Sample allocations remain.
	}
}

TEST(LoopRecorder, PendingParentInputBlocksOnlyItsOwnPhraseFrame) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> backing;
	backing.fill({3200, 3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(backing, silence);

	SampleRecorder parent;
	initialize(parent, 4, source.sample);
	auto* sourceCluster = source.sample->clusters.getElement(0)->cluster;
	sourceCluster->loaded = false;
	std::array<StereoSample, 1> input{{{2560, 0}}};
	parent.processLoopOverdub(std::span(backing).first(1), input);
	CHECK_EQUAL(1, SampleRecorderLifecycleTestAccess::pendingCount(parent));
	sourceCluster->loaded = true; // the asynchronous source read has now completed

	SampleRecorder child;
	initialize(child, 4, parent.sample, 0, &parent);
	CHECK_EQUAL(SampleRecorderLifecycleTestAccess::sourcePending(),
	            SampleRecorderLifecycleTestAccess::sourceStatus(child, 0));
	CHECK_EQUAL(SampleRecorderLifecycleTestAccess::sourceReady(),
	            SampleRecorderLifecycleTestAccess::sourceStatus(child, 3));
}

TEST(LoopRecorder, RejectedAudibleFrameFallsBackToReadableBackingAtTheSamePhase) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> backing;
	backing.fill({3200, 3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(backing, silence);

	SampleRecorder parent;
	initialize(parent, 4, source.sample);
	auto* sourceCluster = source.sample->clusters.getElement(0)->cluster;
	sourceCluster->loaded = false;
	std::array<StereoSample, 1> input{{{2560, 0}}};
	parent.processLoopOverdub(std::span(backing).first(1), input);
	CHECK_EQUAL(1, SampleRecorderLifecycleTestAccess::pendingCount(parent));
	sourceCluster->loaded = true;

	SampleRecorder child;
	initialize(child, 4, parent.sample, 0, &parent);
	std::array<StereoSample, 1> audible{{{777, 888}}};
	deluge::loop_station::renderAudio(audible, std::span(silence).first(1), &child, 1 << 26, 1 << 26, 1 << 24, false,
	                                  [](std::span<StereoSample>) {});
	CHECK_TRUE(child.captureOutputBackingMissing);
	CHECK_EQUAL(0, child.numSamplesCaptured);
	CHECK_EQUAL(3200, audible[0].l);
	CHECK_EQUAL(3200, audible[0].r);
}

TEST(LoopRecorder, PendingInputSerialRangeTracksAppliedFramesAcrossWraps) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> backing;
	backing.fill({3200, 3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(backing, silence);

	SampleRecorder parent;
	initialize(parent, 4, source.sample);
	auto* sourceCluster = source.sample->clusters.getElement(0)->cluster;
	std::array<StereoSample, 3> firstInput;
	firstInput.fill({2560, 0});
	sourceCluster->loaded = false;
	parent.processLoopOverdub(std::span(backing).first(3), firstInput);
	CHECK_EQUAL(3, SampleRecorderLifecycleTestAccess::pendingCount(parent));

	// Apply the first three accepted frames, then queue frames at phrase positions
	// three and zero while the next asynchronous source read is pending.
	sourceCluster->loaded = true;
	parent.processLoopOverdub({}, {});
	CHECK_EQUAL(3, SampleRecorderLifecycleTestAccess::appliedFrames(parent));
	std::array<StereoSample, 2> wrapInput;
	wrapInput.fill({5120, 0});
	sourceCluster->loaded = false;
	parent.processLoopOverdub(std::span(backing).first(2), wrapInput);
	CHECK_EQUAL(5, SampleRecorderLifecycleTestAccess::acceptedFrames(parent));
	CHECK_EQUAL(2, SampleRecorderLifecycleTestAccess::pendingCount(parent));
	sourceCluster->loaded = true;

	SampleRecorder child;
	initialize(child, 4, parent.sample, 0, &parent);
	CHECK_EQUAL(SampleRecorderLifecycleTestAccess::sourceReady(),
	            SampleRecorderLifecycleTestAccess::sourceStatus(child, 1));
	CHECK_EQUAL(SampleRecorderLifecycleTestAccess::sourcePending(),
	            SampleRecorderLifecycleTestAccess::sourceStatus(child, 3));
	CHECK_EQUAL(SampleRecorderLifecycleTestAccess::sourcePending(),
	            SampleRecorderLifecycleTestAccess::sourceStatus(child, 0));
}

TEST(LoopRecorder, AudibleSourceDeadlineFailsInsteadOfPlayingSilence) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> backing;
	backing.fill({3200, 3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(backing, silence);
	SampleRecorder child;
	initialize(child, 4, source.sample);
	source.sample->clusters.getElement(0)->cluster->loaded = false;
	auto bypass = [](std::span<StereoSample>) {};
	deluge::loop_station::renderAudio(backing, silence, &child, 1 << 26, 1 << 26, 1 << 24, false, bypass, false);
	CHECK_TRUE(child.captureOutputBackingMissing);
	CHECK_EQUAL(0, child.numSamplesCaptured);
	source.sample->clusters.getElement(0)->cluster->loaded = true;
}

TEST(LoopRecorder, PublishedPlaybackFailureNeverUnlinksTheDurableWav) {
	int oldUnlinks = unlinks;
	{
		SampleRecorder recorder;
		initialize(recorder, 4);
		recorder.filePathCreated.set("published.wav");
		recorder.retainForCommit();
		recorder.retainLoopReader();
		recorder.status = RecorderStatus::COMPLETE;
		recorder.releaseCommit(true);
		CHECK_TRUE(recorder.isPublished());
		recorder.captureOutputBackingMissing = true;
		CHECK(recorder.cardRoutine() == Error::NONE);
		CHECK(recorder.status == RecorderStatus::COMPLETE);
		recorder.releaseLoopReader();
		CHECK(recorder.status == RecorderStatus::AWAITING_DELETION);
	}
	CHECK_EQUAL(oldUnlinks, unlinks);
}

TEST(LoopRecorder, PlaybackCompletionCountsOneNativePhraseAndResetsOnRetrigger) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> backing{};
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(backing, silence);
	SampleRecorder recorder;
	initialize(recorder, 4, source.sample);
	recorder.processLoopOverdub(std::span(backing).first(3), std::span(silence).first(3));
	CHECK_FALSE(recorder.loopPlaybackEnded());
	recorder.processLoopOverdub(std::span(backing).first(1), std::span(silence).first(1));
	CHECK_TRUE(recorder.loopPlaybackEnded());
	recorder.setLoopPlaybackCursor(2);
	CHECK_FALSE(recorder.loopPlaybackEnded());
}

TEST(LoopRecorder, PendingSourceHasBoundedInputStorageAndReportsExhaustion) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> dry{};
	std::array<StereoSample, 4> input{};
	source.processLoopOverdub(dry, input);
	SampleRecorder child;
	initialize(child, 4, source.sample);
	source.sample->clusters.getElement(0)->cluster->loaded = false;
	for (uint32_t i = 0; i <= Cluster::size; ++i) {
		child.processLoopOverdub(dry, input);
		if (child.terminalError != Error::NONE)
			break;
	}
	CHECK(child.terminalError == Error::INSUFFICIENT_RAM);
	CHECK_FALSE(child.hadCardError);
	CHECK_FALSE(child.captureOutputBackingMissing);
	CHECK_EQUAL(0, child.numSamplesCaptured);
	source.sample->clusters.getElement(0)->cluster->loaded = true;
}

TEST(LoopRecorder, DelayedInputPreservesChronologicalAdditionsAcrossLoopWraps) {
	SampleRecorder source;
	initialize(source, 1);
	std::array<StereoSample, 1> seed{{{3200, 3200}}};
	std::array<StereoSample, 1> silence{};
	source.processLoopOverdub(seed, silence);
	SampleRecorder child;
	initialize(child, 1, source.sample);
	auto* cluster = source.sample->clusters.getElement(0)->cluster;
	cluster->loaded = false;
	std::array<StereoSample, 8> dry{};
	std::array<StereoSample, 8> input;
	input.fill({2560, -2560});
	int allocationsBeforeAudio = liveAllocations;
	child.processLoopOverdub(dry, input);
	CHECK_EQUAL(allocationsBeforeAudio, liveAllocations);
	child.stopLoopOverdubInput();
	cluster->loaded = true;
	child.processLoopOverdub(seed, silence);
	CHECK(child.loopOverdubReady());
	CHECK_EQUAL(5760, seed[0].l);
	CHECK_EQUAL(640, seed[0].r);
	CHECK_EQUAL(allocationsBeforeAudio, liveAllocations);
}

TEST(LoopRecorder, EightTracksRetainAndDrainDelayedInputIndependently) {
	std::array<SampleRecorder, 8> sources;
	std::array<SampleRecorder, 8> children;
	std::array<StereoSample, 4> dry{};
	std::array<StereoSample, 4> silence{};
	std::array<StereoSample, 4> input;
	for (size_t i = 0; i < sources.size(); ++i) {
		initialize(sources[i], 4);
		dry.fill({3200, 3200});
		sources[i].processLoopOverdub(dry, silence);
		initialize(children[i], 4, sources[i].sample);
		sources[i].sample->clusters.getElement(0)->cluster->loaded = false;
		input.fill({int32_t(2560 * (i + 1)), 0});
		children[i].processLoopOverdub(dry, input);
		children[i].stopLoopOverdubInput();
		CHECK_FALSE(children[i].captureOutputBackingMissing);
		CHECK_FALSE(children[i].loopOverdubReady());
	}
	for (size_t i = children.size(); i-- > 0;) {
		sources[i].sample->clusters.getElement(0)->cluster->loaded = true;
		children[i].processLoopOverdub(dry, silence);
		CHECK(children[i].loopOverdubReady());
		CHECK_EQUAL(3200 + 320 * (i + 1), dry[0].l);
		CHECK_EQUAL(3200, dry[0].r);
	}
}

TEST(LoopRecorder, EndingPlaybackFallsBackToReadableSourceWhenWorkingDataIsMissing) {
	SampleRecorder source;
	initialize(source, 4);
	std::array<StereoSample, 4> dry;
	dry.fill({3200, 3200});
	std::array<StereoSample, 4> silence{};
	source.processLoopOverdub(dry, silence);
	SampleRecorder child;
	initialize(child, 4, source.sample);
	std::array<StereoSample, 4> input;
	input.fill({2560, 2560});
	child.processLoopOverdub(dry, input);
	child.stopLoopOverdubInput();
	child.sample->clusters.getElement(0)->cluster->loaded = false;
	dry.fill({3200, 3200});
	child.processLoopOverdub(dry, silence);
	CHECK_TRUE(child.captureOutputBackingMissing);
	CHECK_FALSE(child.hadCardError);
	CHECK_EQUAL(3200, dry[0].l);
	child.sample->clusters.getElement(0)->cluster->loaded = true;
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
	onSeek = [&] {
		CHECK(SampleRecorderLifecycleTestAccess::writeSnapshotAllocated(r));
		r.processLoopOverdub(dry, input);
	};
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

TEST(LoopRecorder, OneShotPartialFinalBufferKeepsRenderedPrefix) {
	std::array<StereoSample, 4> backing;
	backing.fill({3200, -1600});
	std::array<StereoSample, 4> silence{};
	deluge::loop_station::renderAudio(
	    backing, silence, nullptr, 1 << 26, 1 << 26, 1 << 24, false, [](std::span<StereoSample>) {}, true, 3);
	for (size_t i = 0; i < 3; ++i) {
		CHECK_EQUAL(3200, backing[i].l);
		CHECK_EQUAL(-1600, backing[i].r);
	}
	CHECK_EQUAL(0, backing[3].l);
	CHECK_EQUAL(0, backing[3].r);
}

TEST(LoopRecorder, OneShotShorterThanAudioBufferKeepsItsRenderedSamples) {
	std::array<StereoSample, SSI_TX_BUFFER_NUM_SAMPLES> backing;
	backing.fill({3200, -1600});
	std::array<StereoSample, SSI_TX_BUFFER_NUM_SAMPLES> silence{};
	// The voice has ended, but it rendered the first two frames of this buffer.
	deluge::loop_station::renderAudio(
	    backing, silence, nullptr, 1 << 26, 1 << 26, 1 << 24, false, [](std::span<StereoSample>) {}, false, 2);
	CHECK_EQUAL(3200, backing[0].l);
	CHECK_EQUAL(-1600, backing[1].r);
	for (size_t i = 2; i < backing.size(); ++i) {
		CHECK_EQUAL(0, backing[i].l);
		CHECK_EQUAL(0, backing[i].r);
	}
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

TEST(LoopRecorder, SourcePrefetchPrioritizesAudibleFrameAheadOfWriterCursor) {
	uint32_t const audibleFrame = (4 * Cluster::size - 44) / 6 + 2;
	uint32_t const frames = audibleFrame + 8;
	SampleRecorder source;
	initialize(source, frames);
	SampleRecorderLifecycleTestAccess::positions(source, audibleFrame, 0);
	CHECK(SampleRecorderLifecycleTestAccess::prepare(source) == Error::NONE);
	std::array<StereoSample, 1> dry{{{3200, 3200}}}, silence{};
	source.processLoopOverdub(dry, silence);

	SampleRecorder child;
	initialize(child, frames, source.sample);
	child.setLoopPlaybackCursor(audibleFrame);
	Sample* sourceSample = source.sample;
	auto* audibleCluster = sourceSample->clusters.getElement(4)->cluster;
	CHECK(audibleCluster != nullptr);
	uint64_t const sourceOffset = 44 + static_cast<uint64_t>(audibleFrame) * 6;
	std::array<char, 6> persistedFrame{};
	std::copy_n(audibleCluster->data + sourceOffset % Cluster::size, persistedFrame.size(), persistedFrame.begin());
	sourceSample->clusters.getElement(4)->sdAddress = 99;
	recycleReleasedClusters = true;
	SampleRecorderLifecycleTestAccess::releaseLoopCache(source);
	CHECK(sourceSample->clusters.getElement(4)->cluster == nullptr); // The released cache pin evicted this block.
	asyncClusterLoads = true;
	CHECK(SampleRecorderLifecycleTestAccess::prepare(child) == Error::NONE);
	CHECK(SampleRecorderLifecycleTestAccess::sourceCached(child, sourceSample, 4));
	audibleCluster = sourceSample->clusters.getElement(4)->cluster;
	CHECK(audibleCluster != nullptr);
	CHECK_FALSE(audibleCluster->loaded);

	// The loader completed between card service and the next audible buffer.
	std::copy(persistedFrame.begin(), persistedFrame.end(), audibleCluster->data + sourceOffset % Cluster::size);
	audibleCluster->loaded = true;
	std::array<StereoSample, 1> audible{{{0, 0}}};
	deluge::loop_station::renderAudio(audible, std::span(silence).first(1), &child, 1 << 26, 1 << 26, 1 << 24, false,
	                                  [](std::span<StereoSample>) {});
	CHECK_FALSE(child.captureOutputBackingMissing);
	CHECK_EQUAL(3200, audible[0].l);
	CHECK(SampleRecorderLifecycleTestAccess::sourceCacheSize(child) <= 4);
}

TEST(LoopRecorder, SourcePrefetchCoversAudibleBufferAcrossEvictedClusterBoundary) {
	uint32_t constexpr loopFrames = 28000;
	uint32_t constexpr boundaryFrame = (5 * Cluster::size - 44) / 6;
	uint32_t constexpr playbackStart = boundaryFrame - 6;
	SampleRecorder source;
	initialize(source, loopFrames);
	CHECK_EQUAL(6, source.sample->clusters.getNumElements());
	source.sample->clusters.getElement(4)->sdAddress = 99;
	source.sample->clusters.getElement(5)->sdAddress = 199;

	SampleRecorder child;
	initialize(child, loopFrames, source.sample);
	SampleRecorderLifecycleTestAccess::positions(child, 0, playbackStart);
	asyncClusterLoads = true;
	CHECK(SampleRecorderLifecycleTestAccess::prepare(child) == Error::NONE);
	CHECK(SampleRecorderLifecycleTestAccess::sourceCached(child, source.sample, 4));
	CHECK(SampleRecorderLifecycleTestAccess::sourceCached(child, source.sample, 5));

	for (uint32_t index : {4u, 5u}) {
		auto* cluster = source.sample->clusters.getElement(index)->cluster;
		CHECK(cluster != nullptr);
		if (cluster) {
			cluster->loaded = true; // The async loader completed before the audio callback.
			std::fill_n(cluster->data, Cluster::size, 0);
		}
	}
	std::array<StereoSample, SSI_TX_BUFFER_NUM_SAMPLES> backing{};
	std::array<StereoSample, SSI_TX_BUFFER_NUM_SAMPLES> silence{};
	deluge::loop_station::renderAudio(backing, silence, &child, 1 << 26, 1 << 26, 1 << 24, false,
	                                  [](std::span<StereoSample>) {});
	CHECK_FALSE(child.captureOutputBackingMissing);
	CHECK_EQUAL(playbackStart + backing.size(), child.loopPlaybackCursor());
}

TEST(LoopRecorder, DestinationCacheUnpinsOnlyCleanEntriesOutsideRequiredWindows) {
	uint32_t const targetFrame = (8 * Cluster::size - 44) / 6 + 1;
	SampleRecorder recorder;
	initialize(recorder, targetFrame + 16);
	std::array<StereoSample, 1> backing{{{3200, 3200}}}, silence{};
	recorder.processLoopOverdub(backing, silence); // cluster zero is now dirty
	CHECK(SampleRecorderLifecycleTestAccess::destinationDirty(recorder, 0));
	CHECK(SampleRecorderLifecycleTestAccess::destinationCached(recorder, 2));
	CHECK(SampleRecorderLifecycleTestAccess::destinationCached(recorder, 3));
	SampleRecorderLifecycleTestAccess::positions(recorder, targetFrame, 0);
	CHECK(SampleRecorderLifecycleTestAccess::prepare(recorder) == Error::NONE);
	CHECK(SampleRecorderLifecycleTestAccess::destinationCached(recorder, 0));
	CHECK(SampleRecorderLifecycleTestAccess::destinationDirty(recorder, 0));
	CHECK(SampleRecorderLifecycleTestAccess::destinationCached(recorder, 2)); // The window wraps after cluster eight.
	CHECK_FALSE(SampleRecorderLifecycleTestAccess::destinationCached(recorder, 3));
	CHECK(SampleRecorderLifecycleTestAccess::destinationCached(recorder, 8));
}

TEST(LoopRecorder, InFlightDestinationClusterRemainsPinnedDuringWindowRotation) {
	uint32_t const targetFrame = (8 * Cluster::size - 44) / 6 + 1;
	SampleRecorder recorder;
	initialize(recorder, targetFrame + 16);
	std::array<StereoSample, 1> backing{{{3200, 3200}}}, input{{{2560, 0}}};
	recorder.processLoopOverdub(backing, input);
	recorder.file.emplace();
	recorder.filePathCreated.set("test.wav");
	onSeek = [&] {
		CHECK(SampleRecorderLifecycleTestAccess::destinationWriting(recorder, 0));
		SampleRecorderLifecycleTestAccess::positions(recorder, targetFrame, 0);
		CHECK(SampleRecorderLifecycleTestAccess::prepare(recorder) == Error::NONE);
		CHECK(SampleRecorderLifecycleTestAccess::destinationCached(recorder, 0));
	};
	CHECK(recorder.cardRoutine() == Error::NONE);
	onSeek = {};
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
TEST(LoopRecorder, GenerationCommitCoordinatorRejectsReentryAndRollsBackBeforeNotify) {
	bool busy = false;
	int publishes = 0, rollbacks = 0, notifications = 0;
	deluge::loop_station::resolveGenerationCommit(
	    busy, true,
	    [&] {
		    ++publishes;
		    deluge::loop_station::resolveGenerationCommit(
		        busy, true, [&] { return ++publishes != 0; }, [&] { ++rollbacks; }, [&](bool) { ++notifications; });
		    return false;
	    },
	    [&] {
		    CHECK(busy);
		    ++rollbacks;
	    },
	    [&](bool success) {
		    CHECK_FALSE(success);
		    CHECK(busy);
		    CHECK_EQUAL(1, rollbacks);
		    ++notifications;
	    });
	CHECK_FALSE(busy);
	CHECK_EQUAL(1, publishes);
	CHECK_EQUAL(1, rollbacks);
	CHECK_EQUAL(1, notifications);
}

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
	std::array<StereoSample, 4> silence{};
	r.processLoopOverdub(silence, silence);
	r.file.emplace();
	r.filePathCreated.set("test.wav");
	r.status = RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING;
	r.stopLoopOverdubInput();
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
		backing->clusters.getElement(0)->cluster->loaded = false;
		dry.fill({0, 0});
		child->processLoopOverdub(dry, input);
		CHECK_EQUAL(0, child->numSamplesCaptured);
		CHECK_FALSE(child->captureOutputBackingMissing);
		child->stopLoopOverdubInput();
		CHECK_FALSE(child->loopOverdubReady());
		backing->clusters.getElement(0)->cluster->loaded = true;
		child->processLoopOverdub(dry, silence);
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
