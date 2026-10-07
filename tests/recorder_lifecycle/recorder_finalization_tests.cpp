#include "CppUTest/TestHarness.h"
#include "fixture.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"
#include <cstring>

struct SampleRecorderLifecycleTestAccess {
	static Error initialize(SampleRecorder& r) { return r.initializeRecordingSample(); }
	static Error initializeLoopLinear(SampleRecorder& r) {
		r.loopStationLinearCapture_ = true;
		return r.initializeRecordingSample();
	}
	static Error createNextCluster(SampleRecorder& r) { return r.createNextCluster(); }
	static void failCapture(SampleRecorder& r, Error error) { r.failCapture(error); }
	static Error write(SampleRecorder& r) { return r.writeOneCompletedCluster(); }
	static Error finalize(SampleRecorder& r) { return r.finalizeRecordedFile(); }
	static Error trim(SampleRecorder& r, uint32_t bytes) { return r.truncateFileDownToSize(bytes); }
};

using namespace recorder_test;

TEST_GROUP(RecorderFinalization){void setup() override{failure = Failure::NONE;
createError = writeError = finalizeError = Error::NONE;
seekFails = truncateFails = closeFails = headerFails = clusterLoadFails = clusterInsertFails = false;
recycleReleasedClusters = false;
onSeek = onTruncate = onClose = onCreate = onWrite = {};
seeks = truncations = headerWrites = destroyedClusters = clusterReleases = 0;
fileCloses = unlinks = 0;
audioFileManager.clusterBeingLoaded = nullptr;
fileSystem.database = 32;
fileSystem.csize = 64;
}
void teardown() override {
	LONGS_EQUAL(0, liveSamples);
	LONGS_EQUAL(0, liveAllocations);
	LONGS_EQUAL(0, liveClusters);
	CHECK_FALSE(fileOpen);
	onSeek = onTruncate = onClose = onCreate = onWrite = {};
}
// Capture past a cluster boundary, then Smart-crop back before it. The
// startup reasons are acquired by the REAL completed/final-cluster writers.
void prepare(SampleRecorder& r, uint32_t finalBytes = 32000, int channels = 1, bool margins = false,
             bool retainStartup = true) {
	r.keepingReasonsForFirstClusters = retainStartup;
	CHECK(SampleRecorderLifecycleTestAccess::initialize(r) == Error::NONE);
	r.sample->clusters.count = 2;
	auto* tail = r.sample->clusters.getElement(1)->getCluster(r.sample, 1, CLUSTER_DONT_LOAD);
	tail->numReasonsHeldBySampleRecorder++;
	r.currentRecordClusterIndex = 1;
	r.currentRecordCluster = tail;
	r.writePos = tail->data + 1024;
	r.clusterEndPos = tail->data + Cluster::size;
	r.folderID = AudioRecordingFolder::CLIPS;
	r.mode = AudioInputChannel::LEFT;
	r.recordingNumChannels = channels;
	r.recordingExtraMargins = margins;
	r.sample->numChannels = channels;
	r.sample->audioDataStartPosBytes = margins ? 112 : 44;
	// Match a real integral count of 24-bit mono/stereo frames.
	r.sample->audioDataLengthBytes =
	    ((finalBytes - r.sample->audioDataStartPosBytes) / (3 * channels)) * (3 * channels);
	r.sample->lengthInSamples = r.sample->audioDataLengthBytes / (3 * channels);
	r.sample->fileLoopEndSamples = r.sample->lengthInSamples;
	r.numSamplesCaptured = (Cluster::size + 1024 - r.sample->audioDataStartPosBytes) / (3 * channels);
	r.audioDataLengthBytesAsWrittenToFile = 999999;
	r.loopEndSampleAsWrittenToFile = 999999;
	r.capturedTooMuch = true;
	r.file.emplace();
	r.filePathCreated.set("TEMP/test.WAV");
	r.sample->tempFilePathForRecording.set("TEMP/test.WAV");
	r.audioFileNumber = 1;
	r.status = RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING;
	fileSize = Cluster::size + 1024;
	CHECK(SampleRecorderLifecycleTestAccess::write(r) == Error::NONE);
}
void cleanup(SampleRecorder& r) {
	r.releaseCommit(false);
	r.abort();
	CHECK(r.cardRoutine() == Error::NONE);
	POINTERS_EQUAL(nullptr, r.sample);
}
void prepareLoopLinear(SampleRecorder& r, int channels = 1) {
	CHECK(SampleRecorderLifecycleTestAccess::initializeLoopLinear(r) == Error::NONE);
	r.sample->numChannels = channels;
	r.recordingNumChannels = channels;
	r.folderID = AudioRecordingFolder::CLIPS;
	r.mode = AudioInputChannel::LEFT;
	r.writePos = r.currentRecordCluster->data + Cluster::size;
	r.clusterEndPos = r.currentRecordCluster->data + Cluster::size;
}
void allocateLoopClustersThrough(SampleRecorder& r, int totalClusters) {
	for (int i = 1; i < totalClusters; ++i) {
		r.writePos = r.clusterEndPos;
		CHECK(SampleRecorderLifecycleTestAccess::createNextCluster(r) == Error::NONE);
	}
}
}
;

TEST(RecorderFinalization, LoopInitialCaptureAllowsEightUnwrittenClusters) {
	SampleRecorder r;
	prepareLoopLinear(r);
	allocateLoopClustersThrough(r, 8);
	LONGS_EQUAL(8, r.sample->clusters.getNumElements());
	LONGS_EQUAL(7, r.currentRecordClusterIndex);
	cleanup(r);
}

TEST(RecorderFinalization, OrdinaryLinearRecordingIsNotLimitedToEightClusters) {
	SampleRecorder r;
	CHECK(SampleRecorderLifecycleTestAccess::initialize(r) == Error::NONE);
	r.writePos = r.currentRecordCluster->data + Cluster::size;
	r.clusterEndPos = r.currentRecordCluster->data + Cluster::size;
	allocateLoopClustersThrough(r, 10);
	LONGS_EQUAL(10, r.sample->clusters.getNumElements());
	cleanup(r);
}

TEST(RecorderFinalization, MonoAndStereoFramesCanCrossClusterBoundaries) {
	for (int channels : {1, 2}) {
		SampleRecorder r;
		prepareLoopLinear(r, channels);
		allocateLoopClustersThrough(r, 7);
		auto* oldCluster = r.currentRecordCluster;
		int const overshoot = channels == 1 ? 2 : 5;
		for (int i = 0; i < 5; ++i) {
			oldCluster->data[Cluster::size + i] = static_cast<char>(0x40 + i);
		}
		r.writePos = r.clusterEndPos + overshoot;
		CHECK(SampleRecorderLifecycleTestAccess::createNextCluster(r) == Error::NONE);
		LONGS_EQUAL(8, r.sample->clusters.getNumElements());
		LONGS_EQUAL(overshoot, r.writePos - r.currentRecordCluster->data);
		for (int i = 0; i < 5; ++i) {
			LONGS_EQUAL(0x40 + i, r.currentRecordCluster->data[i]);
		}
		cleanup(r);
	}
}

TEST(RecorderFinalization, LoopInitialCaptureRejectsNinthClusterBeforeChangingOwnership) {
	SampleRecorder r;
	prepareLoopLinear(r);
	allocateLoopClustersThrough(r, 8);
	auto* current = r.currentRecordCluster;
	int const liveBefore = liveClusters;
	r.writePos = r.clusterEndPos;
	CHECK(SampleRecorderLifecycleTestAccess::createNextCluster(r) == Error::INSUFFICIENT_RAM);
	LONGS_EQUAL(8, r.sample->clusters.getNumElements());
	LONGS_EQUAL(7, r.currentRecordClusterIndex);
	POINTERS_EQUAL(current, r.currentRecordCluster);
	LONGS_EQUAL(liveBefore, liveClusters);
	CHECK_FALSE(r.hadCardError);
	cleanup(r);
}

TEST(RecorderFinalization, SuccessfulLoopClusterWriteRecoversOneBacklogSlot) {
	SampleRecorder r;
	prepareLoopLinear(r);
	allocateLoopClustersThrough(r, 8);
	CHECK(SampleRecorderLifecycleTestAccess::write(r) == Error::NONE);
	r.writePos = r.clusterEndPos;
	CHECK(SampleRecorderLifecycleTestAccess::createNextCluster(r) == Error::NONE);
	LONGS_EQUAL(9, r.sample->clusters.getNumElements());
	cleanup(r);
}

TEST(RecorderFinalization, FailedLoopClusterWriteDoesNotRecoverBacklogCapacity) {
	SampleRecorder r;
	prepareLoopLinear(r);
	allocateLoopClustersThrough(r, 8);
	writeError = Error::SD_CARD;
	CHECK(SampleRecorderLifecycleTestAccess::write(r) == Error::SD_CARD);
	r.writePos = r.clusterEndPos;
	CHECK(SampleRecorderLifecycleTestAccess::createNextCluster(r) == Error::INSUFFICIENT_RAM);
	LONGS_EQUAL(8, r.sample->clusters.getNumElements());
	CHECK_FALSE(r.hadCardError);
	writeError = Error::NONE;
	cleanup(r);
}

TEST(RecorderFinalization, LoopWriteRemainsChargedDuringReentrantClusterAllocation) {
	SampleRecorder r;
	prepareLoopLinear(r);
	allocateLoopClustersThrough(r, 8);
	Error allocationDuringWrite = Error::NONE;
	int const clusterCountBefore = r.sample->clusters.getNumElements();
	auto* const currentBefore = r.currentRecordCluster;
	int const currentIndexBefore = r.currentRecordClusterIndex;
	onWrite = [&] {
		r.writePos = r.clusterEndPos;
		allocationDuringWrite = SampleRecorderLifecycleTestAccess::createNextCluster(r);
		LONGS_EQUAL(clusterCountBefore, r.sample->clusters.getNumElements());
		POINTERS_EQUAL(currentBefore, r.currentRecordCluster);
		LONGS_EQUAL(currentIndexBefore, r.currentRecordClusterIndex);
	};
	CHECK(SampleRecorderLifecycleTestAccess::write(r) == Error::NONE);
	CHECK(allocationDuringWrite == Error::INSUFFICIENT_RAM);
	CHECK_FALSE(r.hadCardError);
	onWrite = {};
	cleanup(r);
}

TEST(RecorderFinalization, MonoStereoBoundaryAndLateCaptureFailureRollBackWithoutCardError) {
	for (int channels : {1, 2}) {
		SampleRecorder r;
		prepareLoopLinear(r, channels);
		allocateLoopClustersThrough(r, 8);
		auto* const current = r.currentRecordCluster;
		int const currentIndex = r.currentRecordClusterIndex;
		int const overshoot = channels == 1 ? 2 : 5;
		for (int i = 0; i < 5; ++i) {
			current->data[Cluster::size + i] = static_cast<char>(0x20 + i);
		}
		r.writePos = r.clusterEndPos + overshoot;
		CHECK(SampleRecorderLifecycleTestAccess::createNextCluster(r) == Error::INSUFFICIENT_RAM);
		POINTERS_EQUAL(current, r.currentRecordCluster);
		LONGS_EQUAL(currentIndex, r.currentRecordClusterIndex);
		LONGS_EQUAL(8, r.sample->clusters.getNumElements());
		CHECK_FALSE(r.hadCardError);

		SampleRecorderLifecycleTestAccess::failCapture(r, Error::INSUFFICIENT_RAM);
		CHECK(r.status == RecorderStatus::ABORTED);
		CHECK(r.terminalError == Error::INSUFFICIENT_RAM);
		CHECK_FALSE(r.hadCardError);
		CHECK(r.cardRoutine() == Error::NONE);
		POINTERS_EQUAL(nullptr, r.sample);
		LONGS_EQUAL(0, liveClusters);
		LONGS_EQUAL(0, liveAllocations);
	}
}

TEST(RecorderFinalization, SmartTrimReleasesRetainedSecondClusterBeforeDestroyingIt) {
	SampleRecorder r;
	prepare(r);
	CHECK(SampleRecorderLifecycleTestAccess::finalize(r) == Error::NONE);
	LONGS_EQUAL(1, r.sample->clusters.getNumElements());
	LONGS_EQUAL(1, destroyedClusters);
	LONGS_EQUAL(1, r.sample->clusters.getElement(0)->cluster->numReasonsHeldBySampleRecorder);
	LONGS_EQUAL(r.sample->audioDataStartPosBytes + r.sample->audioDataLengthBytes, fileSize);
	LONGS_EQUAL(1, headerWrites);
	uint32_t dataLength;
	std::memcpy(&dataLength, r.sample->clusters.getElement(0)->cluster->data + 40, sizeof(dataLength));
	UNSIGNED_LONGS_EQUAL(r.sample->audioDataLengthBytes, dataLength);
	uint32_t riffLength;
	std::memcpy(&riffLength, r.sample->clusters.getElement(0)->cluster->data + 4, sizeof(riffLength));
	UNSIGNED_LONGS_EQUAL(fileSize - 8, riffLength);
	cleanup(r);
	LONGS_EQUAL(2, destroyedClusters);
}

TEST(RecorderFinalization, MonoStereoAndMarginsRetainTheCorrectWavEndpoint) {
	for (int channels : {1, 2})
		for (bool margins : {false, true}) {
			SampleRecorder r;
			prepare(r, 32000, channels, margins);
			CHECK(SampleRecorderLifecycleTestAccess::finalize(r) == Error::NONE);
			LONGS_EQUAL(channels, r.sample->numChannels);
			LONGS_EQUAL(r.sample->audioDataStartPosBytes + r.sample->audioDataLengthBytes, fileSize);
			if (margins) {
				uint32_t endpoint;
				std::memcpy(&endpoint, r.sample->clusters.getElement(0)->cluster->data + 92, sizeof(endpoint));
				UNSIGNED_LONGS_EQUAL(r.sample->fileLoopEndSamples, endpoint);
			}
			cleanup(r);
		}
}

TEST(RecorderFinalization, FailedSeekAndTruncatePreserveDescriptorsAndCleanUpOnce) {
	for (bool seek : {false, true}) {
		SampleRecorder r;
		prepare(r);
		seekFails = seek;
		truncateFails = !seek;
		CHECK(SampleRecorderLifecycleTestAccess::finalize(r) == Error::SD_CARD);
		LONGS_EQUAL(2, r.sample->clusters.getNumElements());
		LONGS_EQUAL(0, r.sample->clusters.getElement(1)->cluster->numReasonsHeldBySampleRecorder);
		cleanup(r);
		seekFails = truncateFails = false;
	}
}

TEST(RecorderFinalization, PlaybackAndLoaderReferencesRejectTrimWithoutDestroyingBlocks) {
	for (bool loading : {false, true}) {
		SampleRecorder r;
		prepare(r);
		auto* tail = r.currentRecordCluster;
		tail->numReasonsToBeLoaded++;
		if (loading)
			audioFileManager.clusterBeingLoaded = tail;
		CHECK(SampleRecorderLifecycleTestAccess::finalize(r) == Error::BUG);
		LONGS_EQUAL(2, r.sample->clusters.getNumElements());
		LONGS_EQUAL(0, seeks);
		LONGS_EQUAL(0, tail->numReasonsHeldBySampleRecorder);
		audioFileManager.clusterBeingLoaded = nullptr;
		audioFileManager.removeReasonFromCluster(*tail, "playback rollback");
		cleanup(r);
	}
}

TEST(RecorderFinalization, StorageYieldsRevalidateOwnersBeforeDestroyingDescriptors) {
	for (bool duringSeek : {false, true}) {
		SampleRecorder r;
		prepare(r);
		auto* tail = r.currentRecordCluster;
		auto acquire = [tail] { tail->numReasonsToBeLoaded++; };
		if (duringSeek)
			onSeek = acquire;
		else
			onTruncate = acquire;
		CHECK(SampleRecorderLifecycleTestAccess::finalize(r) == Error::BUG);
		LONGS_EQUAL(2, r.sample->clusters.getNumElements());
		onSeek = onTruncate = {};
		audioFileManager.removeReasonFromCluster(*tail, "yield rollback");
		cleanup(r);
	}
}

TEST(RecorderFinalization, HeaderLoadWriteAndCloseFailuresReachCommitRollback) {
	for (int failureIndex = 0; failureIndex < 3; ++failureIndex) {
		SampleRecorder r;
		prepare(r);
		r.retainForCommit();
		r.sample->addReason(); // staged owner
		clusterLoadFails = failureIndex == 0;
		headerFails = failureIndex == 1;
		closeFails = failureIndex == 2;
		CHECK(r.cardRoutine() == Error::SD_CARD);
		CHECK(r.status == RecorderStatus::COMPLETE);
		CHECK(r.terminalError == Error::SD_CARD);
		CHECK(r.sample != nullptr);
		r.sample->removeReason("rollback staged");
		clusterLoadFails = headerFails = closeFails = false;
		cleanup(r);
	}
}

TEST(RecorderFinalization, ExactBoundaryAndNoTrimPreserveStartupReferences) {
	for (uint32_t bytes : {32767u, 32768u, 32769u}) {
		SampleRecorder r;
		prepare(r, 33000);
		r.capturedTooMuch = false;
		CHECK(SampleRecorderLifecycleTestAccess::finalize(r) == Error::NONE);
		LONGS_EQUAL(2, r.sample->clusters.getNumElements());
		r.file.emplace();
		CHECK(SampleRecorderLifecycleTestAccess::trim(r, bytes) == Error::NONE);
		LONGS_EQUAL(bytes > Cluster::size ? 2 : 1, r.sample->clusters.getNumElements());
		cleanup(r);
	}
}

TEST(RecorderFinalization, InvalidTrimAndIncompleteCaptureAreRecoverable) {
	SampleRecorder r;
	prepare(r);
	CHECK(SampleRecorderLifecycleTestAccess::trim(r, 32000) == Error::BUG);
	CHECK(SampleRecorderLifecycleTestAccess::finalize(r) == Error::NONE);
	r.file.emplace();
	for (uint32_t bytes : {0u, 1u, 43u, 65536u}) {
		CHECK(SampleRecorderLifecycleTestAccess::trim(r, bytes) == Error::BUG);
	}
	cleanup(r);
}

TEST(RecorderFinalization, FourRapidPendingCommitsRepeatWithoutRetainedRecorderResources) {
	for (int cycle = 0; cycle < 1000; ++cycle) {
		SampleRecorder recorders[4];
		for (auto& r : recorders) {
			prepare(r);
			r.retainForCommit();
		}
		for (auto& r : recorders) {
			CHECK(r.cardRoutine() == Error::NONE);
			CHECK(r.status == RecorderStatus::COMPLETE);
			r.releaseCommit(true);
			CHECK(r.status == RecorderStatus::AWAITING_DELETION);
		}
		// The fixtures have no durable clip/history owners; recorder destruction
		// frees the successful unregistered samples at the end of each round.
	}
}

TEST(RecorderFinalization, FailedFinalClusterWriteAndOverhangAllocationReleaseEveryReason) {
	for (bool overhang : {false, true}) {
		SampleRecorder r;
		prepare(r);
		r.retainForCommit();
		if (overhang) {
			r.writePos = r.clusterEndPos + 3;
			clusterInsertFails = true;
		}
		else
			writeError = Error::SD_CARD;
		Error expected = overhang ? Error::INSUFFICIENT_RAM : Error::SD_CARD;
		CHECK(r.cardRoutine() == expected);
		CHECK(r.terminalError == expected);
		cleanup(r);
		writeError = Error::NONE;
		clusterInsertFails = false;
	}
}

TEST(RecorderFinalization, AbortAfterCompletedStartupWriteAndBeforeFinalizationIsBalanced) {
	SampleRecorder r;
	prepare(r);
	r.retainForCommit();
	r.sample->addReason();
	r.abort();
	CHECK(r.cardRoutine() == Error::NONE);
	LONGS_EQUAL(2, liveClusters);
	r.sample->removeReason("rollback");
	cleanup(r);
	LONGS_EQUAL(2, destroyedClusters);
}

TEST(RecorderFinalization, NonRetainingRecorderAndRepeatedTrimAreSafe) {
	SampleRecorder r;
	prepare(r, 32000, 1, false, false);
	CHECK(SampleRecorderLifecycleTestAccess::finalize(r) == Error::NONE);
	r.file.emplace();
	CHECK(SampleRecorderLifecycleTestAccess::trim(r, 32000) == Error::NONE);
	LONGS_EQUAL(1, destroyedClusters);
	cleanup(r);
}

TEST(RecorderFinalization, ReleasingTheTailMayImmediatelyRecycleItsCluster) {
	SampleRecorder r;
	prepare(r);
	recycleReleasedClusters = true;
	CHECK(SampleRecorderLifecycleTestAccess::finalize(r) == Error::NONE);
	LONGS_EQUAL(1, destroyedClusters);
	LONGS_EQUAL(1, r.sample->clusters.getNumElements());
	cleanup(r);
	LONGS_EQUAL(2, destroyedClusters);
}

TEST(RecorderFinalization, HeaderOnlyTrimKeepsTheHeaderCluster) {
	SampleRecorder r;
	prepare(r);
	CHECK(SampleRecorderLifecycleTestAccess::finalize(r) == Error::NONE);
	r.file.emplace();
	CHECK(SampleRecorderLifecycleTestAccess::trim(r, r.sample->audioDataStartPosBytes) == Error::NONE);
	LONGS_EQUAL(1, r.sample->clusters.getNumElements());
	cleanup(r);
}

TEST(RecorderFinalization, PlaybackOfRetainedAudioSurvivesTrimAndRecorderDestruction) {
	Sample* durable = nullptr;
	Cluster* playing = nullptr;
	{
		SampleRecorder r;
		prepare(r);
		durable = r.sample;
		durable->addReason(); // durable clip/history owner
		playing = durable->clusters.getElement(0)->cluster;
		playing->numReasonsToBeLoaded++;
		r.retainForCommit();
		CHECK(r.cardRoutine() == Error::NONE);
		r.releaseCommit(true);
	}
	LONGS_EQUAL(1, liveSamples);
	LONGS_EQUAL(1, liveClusters);
	LONGS_EQUAL(1, playing->numReasonsToBeLoaded);
	LONGS_EQUAL(0, playing->numReasonsHeldBySampleRecorder);
	audioFileManager.removeReasonFromCluster(*playing, "stop playback");
	durable->removeReason("unload durable clip");
	audioFileManager.deleteUnusedAudioFileFromMemoryIndexUnknown(*durable);
}
