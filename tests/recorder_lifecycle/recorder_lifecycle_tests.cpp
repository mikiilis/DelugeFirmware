#include "CppUTest/TestHarness.h"
#include "fixture.h"
#include "model/output.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "storage/audio/audio_file_manager.h"

struct SampleRecorderLifecycleTestAccess {
	static Error initialize(SampleRecorder& recorder) { return recorder.initializeRecordingSample(); }
	static Error clean(SampleRecorder& recorder) { return recorder.cleanupAbortedRecording(); }
};

using namespace recorder_test;

TEST_GROUP(RecorderLifecycle){void setup() override{failure = Failure::NONE;
clusterReleases = fileCloses = unlinks = 0;
fileOpen = false;
onClose = {};
onCreate = {};
createError = writeError = finalizeError = Error::NONE;
}
void teardown() override {
	LONGS_EQUAL(0, liveSamples);
	LONGS_EQUAL(0, liveAllocations);
	onClose = {};
	onCreate = {};
}
void initialize(SampleRecorder& recorder, bool registered = true) {
	CHECK(SampleRecorderLifecycleTestAccess::initialize(recorder) == Error::NONE);
	recorder.haveAddedSampleToArray = registered;
	recorder.folderID = AudioRecordingFolder::CLIPS;
	recorder.mode = AudioInputChannel::LEFT;
}
void clean(SampleRecorder& recorder) {
	recorder.abort();
	CHECK(recorder.cardRoutine() == Error::NONE);
}
}
;

TEST(RecorderLifecycle, DefaultRecorderDestructionHasNoOutputOrSample) {
	SampleRecorder recorder;
	POINTERS_EQUAL(nullptr, recorder.sample);
	POINTERS_EQUAL(nullptr, recorder.outputRecordingFrom);
}

TEST(RecorderLifecycle, EverySetupAllocationFailureIsSafeToDestroy) {
	for (auto injection : {Failure::SAMPLE_ALLOCATION, Failure::SAMPLE_INITIALIZE, Failure::FIRST_CLUSTER}) {
		failure = injection;
		{
			SampleRecorder recorder;
			CHECK(SampleRecorderLifecycleTestAccess::initialize(recorder) == Error::INSUFFICIENT_RAM);
			POINTERS_EQUAL(nullptr, recorder.sample);
			POINTERS_EQUAL(nullptr, recorder.currentRecordCluster);
		}
		LONGS_EQUAL(0, liveAllocations);
		LONGS_EQUAL(0, liveSamples);
	}
}

TEST(RecorderLifecycle, RejectedAttachmentDoesNotRemoveExistingRecorder) {
	Output output;
	SampleRecorder existing;
	existing.outputRecordingFrom = &output;
	output.recorder = &existing;
	{
		SampleRecorder rejected;
		initialize(rejected, false);
		rejected.outputRecordingFrom = &output;
	}
	POINTERS_EQUAL(&existing, output.recorder);
	CHECK(existing.status == RecorderStatus::CAPTURING_DATA);
}

TEST(RecorderLifecycle, CompletedOutputCanBeDetachedThenDestroyed) {
	Output output;
	{
		SampleRecorder recorder;
		recorder.outputRecordingFrom = &output;
		output.recorder = &recorder;
		recorder.status = RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING;
		CHECK(output.removeRecorder(&recorder));
		POINTERS_EQUAL(nullptr, recorder.outputRecordingFrom);
		CHECK_FALSE(output.removeRecorder(&recorder));
	}
	POINTERS_EQUAL(nullptr, output.recorder);
}

TEST(RecorderLifecycle, AbortBeforeRegistrationReleasesSampleAndCluster) {
	SampleRecorder recorder;
	initialize(recorder, false);
	clean(recorder);
	POINTERS_EQUAL(nullptr, recorder.sample);
	LONGS_EQUAL(1, clusterReleases);
	LONGS_EQUAL(0, liveSamples);
}

TEST(RecorderLifecycle, CardCleanupWaitsForProvisionalOwners) {
	SampleRecorder recorder;
	initialize(recorder);
	auto* sample = recorder.sample;
	sample->addReason(); // provisional clip
	sample->addReason(); // provisional history
	recorder.retainForCommit();
	recorder.abort(); // allocation failed while capturing the latency tail
	clean(recorder);  // writer runs before Loop Station polls the abort
	POINTERS_EQUAL(sample, recorder.sample);
	LONGS_EQUAL(3, sample->numReasonsToBeLoaded);
	LONGS_EQUAL(0, clusterReleases);
	CHECK(recorder.status == RecorderStatus::ABORTED);
	CHECK(recorder.pointerHeldElsewhere);
	sample->removeReason("clip rollback");
	sample->removeReason("history rollback");
	recorder.releaseCommit(false);
	clean(recorder);
	POINTERS_EQUAL(nullptr, recorder.sample);
	CHECK(recorder.status == RecorderStatus::AWAITING_DELETION);
	LONGS_EQUAL(1, clusterReleases);
}

TEST(RecorderLifecycle, OwnerRollbackBeforeCardCleanupIsAlsoSafe) {
	SampleRecorder recorder;
	initialize(recorder);
	recorder.retainForCommit();
	recorder.sample->addReason();
	recorder.abort();
	recorder.sample->removeReason("rollback");
	recorder.releaseCommit(false);
	clean(recorder);
	LONGS_EQUAL(0, liveSamples);
}

TEST(RecorderLifecycle, AbortClosesFileBeforeUnlinkAndOnlyAfterRollback) {
	SampleRecorder recorder;
	initialize(recorder);
	recorder.file.emplace();
	recorder.filePathCreated.set("TEMP/short.WAV");
	recorder.audioFileNumber = 1;
	recorder.retainForCommit();
	recorder.abort();
	clean(recorder);
	LONGS_EQUAL(0, fileCloses);
	LONGS_EQUAL(0, unlinks);
	recorder.releaseCommit(false);
	// A close can yield to audio; the recorder/sample must still be valid then.
	onClose = [&] {
		CHECK(recorder.sample != nullptr);
		CHECK(recorder.status == RecorderStatus::ABORTED);
	};
	clean(recorder);
	LONGS_EQUAL(1, fileCloses);
	LONGS_EQUAL(1, unlinks);
	onClose = {};
}

TEST(RecorderLifecycle, RepeatedRollbackAndCleanupDoNotReleaseTwice) {
	SampleRecorder recorder;
	initialize(recorder);
	recorder.retainForCommit();
	recorder.releaseCommit(false);
	recorder.releaseCommit(false);
	clean(recorder);
	clean(recorder);
	LONGS_EQUAL(1, clusterReleases);
}

TEST(RecorderLifecycle, SuccessfulCommitKeepsDurableSampleAliveAfterRecorderDestruction) {
	Sample* durable;
	{
		SampleRecorder recorder;
		initialize(recorder);
		durable = recorder.sample;
		durable->addReason();
		recorder.retainForCommit();
		recorder.status = RecorderStatus::COMPLETE;
		recorder.releaseCommit(true);
		CHECK(recorder.status == RecorderStatus::AWAITING_DELETION);
		CHECK_FALSE(recorder.pointerHeldElsewhere);
	}
	LONGS_EQUAL(1, durable->numReasonsToBeLoaded);
	LONGS_EQUAL(1, clusterReleases);
	durable->removeReason("history cleared");
	audioFileManager.deleteUnusedAudioFileFromMemoryIndexUnknown(*durable);
}

TEST(RecorderLifecycle, OrdinaryRecorderPointerDoesNotDeferAbortSampleCleanup) {
	SampleRecorder recorder;
	initialize(recorder);
	recorder.pointerHeldElsewhere = true;
	recorder.abort();
	clean(recorder);
	POINTERS_EQUAL(nullptr, recorder.sample);
	CHECK(recorder.status == RecorderStatus::ABORTED);
	recorder.pointerHeldElsewhere = false;
	clean(recorder);
	CHECK(recorder.status == RecorderStatus::AWAITING_DELETION);
}

TEST(RecorderLifecycle, StorageFailuresKeepTheirOriginalErrorDuringRollback) {
	// File creation, write, header update and close failures all enter rollback
	// through terminalError/hadCardError, regardless of whether capture has ended.
	for (auto error : {Error::SD_CARD_FULL, Error::SD_CARD_NOT_PRESENT, Error::WRITE_PROTECTED, Error::SD_CARD}) {
		SampleRecorder recorder;
		initialize(recorder);
		recorder.retainForCommit();
		recorder.terminalError = error;
		recorder.hadCardError = true;
		recorder.status = RecorderStatus::COMPLETE;
		recorder.releaseCommit(false);
		clean(recorder);
		CHECK(recorder.terminalError == error);
	}
}

TEST(RecorderLifecycle, FileCreationFailureReachesOwnerWithoutDeletingProvisionalSample) {
	SampleRecorder recorder;
	initialize(recorder, false);
	recorder.retainForCommit();
	recorder.sample->addReason();
	recorder.status = RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING;
	createError = Error::SD_CARD_FULL;
	CHECK(recorder.cardRoutine() == Error::SD_CARD_FULL);
	CHECK(recorder.status == RecorderStatus::COMPLETE);
	CHECK(recorder.terminalError == Error::SD_CARD_FULL);
	CHECK(recorder.sample != nullptr);
	recorder.sample->removeReason("rollback");
	recorder.releaseCommit(false);
	clean(recorder);
	LONGS_EQUAL(0, unlinks);
}

TEST(RecorderLifecycle, ClusterWriteFailureReachesOwnerAndReleasesWrittenClusterOnce) {
	SampleRecorder recorder;
	initialize(recorder, false);
	recorder.retainForCommit();
	// First cluster is full; no second cluster was allocated.
	recorder.currentRecordClusterIndex = 1;
	recorder.currentRecordCluster = nullptr;
	recorder.status = RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING;
	writeError = Error::SD_CARD;
	CHECK(recorder.cardRoutine() == Error::SD_CARD);
	CHECK(recorder.status == RecorderStatus::COMPLETE);
	CHECK(recorder.terminalError == Error::SD_CARD);
	recorder.releaseCommit(false);
	clean(recorder);
	LONGS_EQUAL(1, clusterReleases);
	LONGS_EQUAL(1, unlinks);
}

TEST(RecorderLifecycle, FinalizationFailureIsNotPublishedAsSuccessfulCompletion) {
	SampleRecorder recorder;
	initialize(recorder, false);
	recorder.retainForCommit();
	recorder.status = RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING;
	finalizeError = Error::SD_CARD;
	CHECK(recorder.cardRoutine() == Error::SD_CARD);
	CHECK(recorder.status == RecorderStatus::COMPLETE);
	CHECK(recorder.hadCardError);
	CHECK(recorder.terminalError == Error::SD_CARD);
	recorder.releaseCommit(false);
	clean(recorder);
	LONGS_EQUAL(1, fileCloses);
	LONGS_EQUAL(1, unlinks);
}

TEST(RecorderLifecycle, AbortWhileFileCreationYieldsDefersTheAbortedLabelCleanup) {
	SampleRecorder recorder;
	initialize(recorder, false);
	recorder.retainForCommit();
	recorder.sample->addReason();
	onCreate = [&] { recorder.abort(); };
	CHECK(recorder.cardRoutine() == Error::NONE);
	CHECK(recorder.status == RecorderStatus::ABORTED);
	CHECK(recorder.sample != nullptr);
	LONGS_EQUAL(0, fileCloses);
	LONGS_EQUAL(0, unlinks);
	recorder.sample->removeReason("rollback");
	recorder.releaseCommit(false);
	clean(recorder);
	LONGS_EQUAL(1, fileCloses);
	LONGS_EQUAL(1, unlinks);
	onCreate = {};
}

TEST(RecorderLifecycle, RapidAbortsAcrossEightSlotsDoNotAccumulateResources) {
	for (int cycle = 0; cycle < 1000; ++cycle) {
		std::array<SampleRecorder, 8> recorders;
		for (auto& recorder : recorders) {
			initialize(recorder, cycle % 2 == 0);
			recorder.retainForCommit();
			recorder.sample->addReason();
			recorder.abort();
			clean(recorder);
		}
		for (auto& recorder : recorders) {
			recorder.sample->removeReason("rollback");
			recorder.releaseCommit(false);
			clean(recorder);
		}
		LONGS_EQUAL(0, liveSamples);
		LONGS_EQUAL(0, liveAllocations);
	}
}
