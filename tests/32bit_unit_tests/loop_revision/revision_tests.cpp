#include "CppUTest/TestHarness.h"
#include "loop_station/revision.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/audio_clip.h"
#include <array>
#include <cstdio>
#include <cstdlib>

static_assert(sizeof(void*) == 4, "String reasons and allocator headers require 32-bit pointers");
TEST_GROUP(LoopRevisionOwnership) {
	std::array<void*, NUM_MEMORY_REGIONS> memory{};
	std::array<std::array<uint32_t, 2048>, NUM_MEMORY_REGIONS> records{};
	void setup() override {
		auto& allocator = GeneralMemoryAllocator::get();
		for (size_t i = 0; i < memory.size(); ++i) {
			memory[i] = malloc(1024 * 1024);
			CHECK(memory[i]);
			auto& region = allocator.regions[i];
			// Reset metadata before installing each test's independently owned arena.
			region.~MemoryRegion();
			new (&region) MemoryRegion();
			region.setup(records[i].data(), sizeof(records[i]), reinterpret_cast<uint32_t>(memory[i]),
			             reinterpret_cast<uint32_t>(memory[i]) + 1024 * 1024, nullptr);
		}
	}
	void teardown() override {
		for (void* p : memory)
			free(p);
	}
};

TEST(LoopRevisionOwnership, RepeatedPublicationGrowthRenameAndBranchKeepStringsAndReasonsAlive) {
	std::array<Sample, 102> samples;
	AudioClip clip;
	LoopStationRevisionHistory history;
	for (int i = 0; i < 101; ++i) {
		char path[64];
		snprintf(path, sizeof(path), "SAMPLES/CLIPS/TEMP/REC%05d.WAV", i);
		CHECK(samples[i].filePath.set(path) == Error::NONE);
		CHECK(history.prepareAppend(clip, i != 0));
		clip.sampleHolder.setAudioFile(&samples[i]);
		CHECK(history.stageCurrent(clip));
		CHECK_FALSE(history.prepareAppend(clip, true)); // Publication is delayed.
		CHECK_EQUAL(2, samples[i].reasons);             // Clip and provisional ownership.
		// Finalization changes the path after staging; old clones must stay valid.
		snprintf(path, sizeof(path), "SAMPLES/CLIPS/REC%05d.WAV", i);
		CHECK(samples[i].filePath.set(path) == Error::NONE);
		CHECK(history.publishStaged(clip));
		STRCMP_EQUAL(path, history.current()->filePath.get());
		CHECK_EQUAL(i + 1, history.revisions.size());
		CHECK_EQUAL(2, samples[i].reasons);
		for (int j = 0; j < i; ++j)
			CHECK_EQUAL(1, samples[j].reasons);
	}
	CHECK(history.navigate(clip, false) == Error::NONE);
	CHECK(history.prepareAppend(clip, true));
	CHECK(samples[101].filePath.set("SAMPLES/CLIPS/BRANCH.WAV") == Error::NONE);
	clip.sampleHolder.setAudioFile(&samples[101]);
	CHECK(history.stageCurrent(clip));
	CHECK(history.rejectStaged(clip) == Error::NONE);
	CHECK_EQUAL(0, samples[101].reasons);
	CHECK_TRUE(history.canRedo());
	CHECK(history.navigate(clip, true) == Error::NONE);
	CHECK(history.navigate(clip, false) == Error::NONE);
	CHECK(history.prepareAppend(clip, true));
	clip.sampleHolder.setAudioFile(&samples[101]);
	CHECK(history.stageCurrent(clip));
	CHECK(history.publishStaged(clip));
	CHECK_FALSE(history.canRedo());
	CHECK_EQUAL(0, samples[100].reasons);
	history.clear();
	clip.sampleHolder.setAudioFile(nullptr);
	for (auto& sample : samples)
		CHECK_EQUAL(0, sample.reasons);
}

TEST(LoopRevisionOwnership, CopyMoveAndSelfAssignmentPreserveSharedPathAndSingleReasonPerOwner) {
	Sample sample;
	CHECK(sample.filePath.set("SAMPLES/CLIPS/REC.WAV") == Error::NONE);
	{
		LoopStationRevision first;
		first.filePath.set(&sample.filePath);
		first.setSample(&sample);
		LoopStationRevision copy(first);
		LoopStationRevision moved(std::move(copy));
		first = first;
		moved = std::move(moved);
		CHECK_EQUAL(2, sample.reasons);
		CHECK(copy.sample == nullptr);
		sample.filePath.clear();
		STRCMP_EQUAL("SAMPLES/CLIPS/REC.WAV", moved.filePath.get());
		copy = first;
		CHECK_EQUAL(3, sample.reasons);
		moved = std::move(copy);
		CHECK_EQUAL(2, sample.reasons);
	}
	CHECK_EQUAL(0, sample.reasons);
}

TEST(LoopRevisionOwnership, GeneralExternalStringCanBeClonedOutsideSmallRegion) {
	std::array<char, 512> path;
	path.fill('x');
	path.back() = 0;
	String original;
	CHECK(original.set(path.data()) == Error::NONE);
	CHECK_EQUAL(MEMORY_REGION_EXTERNAL, GeneralMemoryAllocator::get().getRegion(const_cast<char*>(original.get())));
	String clone(original);
	original.clear();
	STRCMP_EQUAL(path.data(), clone.get());
}
