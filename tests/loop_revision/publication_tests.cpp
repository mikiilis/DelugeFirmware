#include "CppUTest/TestHarness.h"
#include "loop_station/revision.h"
#include "model/clip/audio_clip.h"
#include <array>

TEST_GROUP(LoopRevisionPublication){};

TEST(LoopRevisionPublication, OlderCompletionsAppendInOrderWithoutReplacingTheLiveClip) {
	std::array<Sample, 3> samples;
	AudioClip clip;
	LoopStationRevisionHistory history;
	std::array<LoopStationRevision, 3> pending;
	for (size_t i = 0; i < samples.size(); ++i) {
		CHECK(history.prepareAppend(clip, false, i));
		clip.sampleHolder.setAudioFile(&samples[i]);
		clip.loopStationSampleZeroFrames = i * 100;
		pending[i].capture(clip);
	}
	CHECK_EQUAL(0, history.revisions.size());
	for (size_t i = 0; i < pending.size(); ++i) {
		samples[i].filePath.set(i == 0 ? "first.wav" : i == 1 ? "second.wav" : "third.wav");
		CHECK(history.publishGeneration(std::move(pending[i])));
		POINTERS_EQUAL(&samples[2], clip.sampleHolder.audioFile);
		CHECK_EQUAL(200, clip.loopStationSampleZeroFrames);
		CHECK_EQUAL(i + 1, history.revisions.size());
		POINTERS_EQUAL(&samples[i], history.current()->sample);
	}
	CHECK(history.navigate(clip, false) == Error::NONE);
	POINTERS_EQUAL(&samples[1], clip.sampleHolder.audioFile);
	CHECK_EQUAL(100, clip.loopStationSampleZeroFrames);
	CHECK(history.navigate(clip, false) == Error::NONE);
	POINTERS_EQUAL(&samples[0], clip.sampleHolder.audioFile);
	CHECK(history.navigate(clip, true) == Error::NONE);
	POINTERS_EQUAL(&samples[1], clip.sampleHolder.audioFile);
}

TEST(LoopRevisionPublication, FailedGenerationCanRollBackWithoutPublishingOrDestroyingRedo) {
	std::array<Sample, 3> samples;
	AudioClip clip;
	LoopStationRevisionHistory history;
	for (size_t i = 0; i < 2; ++i) {
		CHECK(history.prepareAppend(clip, false));
		clip.sampleHolder.setAudioFile(&samples[i]);
		LoopStationRevision revision;
		revision.capture(clip);
		CHECK(history.publishGeneration(std::move(revision)));
	}
	CHECK(history.navigate(clip, false) == Error::NONE);
	CHECK(history.prepareAppend(clip, true));
	{
		clip.sampleHolder.setAudioFile(&samples[2]);
		LoopStationRevision rejected;
		rejected.capture(clip);
		CHECK(history.restoreCurrent(clip) == Error::NONE);
	}
	CHECK_EQUAL(0, samples[2].reasons);
	CHECK_EQUAL(2, history.revisions.size());
	CHECK(history.canRedo());
	POINTERS_EQUAL(&samples[0], clip.sampleHolder.audioFile);
}

TEST(LoopRevisionPublication, FailedDurableRestoreDetachesRejectedSampleAndKeepsHistoryForRetry) {
	std::array<Sample, 2> samples;
	AudioClip clip;
	LoopStationRevisionHistory history;
	history.revisions.emplace_back();
	history.revisions.back().filePath.set("missing-durable.wav");
	history.revisions.emplace_back();
	history.revisions.back().capture(clip);
	history.cursor = 0;
	clip.sampleHolder.setAudioFile(&samples[1]);
	clip.sampleHolder.filePath.set("rejected-working.wav");
	clip.sampleHolder.startPos = 12;
	clip.sampleHolder.endPos = 345;
	clip.loopStationSampleZeroFrames = 234;

	CHECK(history.restoreCurrent(clip) == Error::FILE_NOT_FOUND);
	POINTERS_EQUAL(nullptr, clip.sampleHolder.audioFile);
	CHECK(clip.sampleHolder.filePath.isEmpty());
	CHECK_EQUAL(0, clip.sampleHolder.startPos);
	CHECK_EQUAL(0, clip.sampleHolder.endPos);
	CHECK_EQUAL(0, clip.loopStationSampleZeroFrames);
	CHECK_EQUAL(2, history.revisions.size());
	CHECK(history.canRedo());
	STRCMP_EQUAL("missing-durable.wav", history.current()->filePath.get());
	CHECK_EQUAL(0, samples[1].reasons);
}
