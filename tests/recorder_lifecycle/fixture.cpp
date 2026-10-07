#include "fixture.h"
#include "fatfs/diskio.h"
#include "fatfs/ff.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "util/d_string.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>

GeneralMemoryAllocator& GeneralMemoryAllocator::get() {
	static GeneralMemoryAllocator allocator;
	return allocator;
}
void* GeneralMemoryAllocator::allocStealable(size_t size) {
	if (recorder_test::failure == recorder_test::Failure::SAMPLE_ALLOCATION)
		return nullptr;
	auto* result = std::malloc(size);
	if (result)
		recorder_test::liveAllocations++;
	return result;
}
void delugeDealloc(void* memory) {
	if (memory)
		recorder_test::liveAllocations--;
	std::free(memory);
}

extern const char nothing = '\0';
String::~String() {
	clear();
}
void String::clear(bool) {
	std::free(stringMemory);
	stringMemory = nullptr;
}
Error String::set(char const* text, int32_t length) {
	clear();
	if (length < 0)
		length = std::strlen(text);
	stringMemory = static_cast<char*>(std::malloc(length + 1));
	std::memcpy(stringMemory, text, length);
	stringMemory[length] = '\0';
	return Error::NONE;
}
void String::set(String const* other) {
	set(other->get());
}

// Storage boundaries below the production cardRoutine are injectable.
#ifndef RECORDER_FINALIZATION_TESTS
Error SampleRecorder::writeOneCompletedCluster() {
	return recorder_test::writeError;
}
Error SampleRecorder::finalizeRecordedFile() {
	return recorder_test::finalizeError;
}
#else
Error SampleRecorder::writeCluster(int32_t index, size_t) {
	if (recorder_test::onWrite)
		recorder_test::onWrite();
	sample->clusters.getElement(index)->sdAddress = 32 + index * 64;
	return recorder_test::writeError;
}
Error SampleRecorder::alterFile(MonitoringAction, int32_t, uint32_t, uint64_t) {
	throw std::runtime_error("File alteration is disabled for Loop Station");
}
bool SampleRecorder::inputHasNoRightChannel() {
	return false;
}
bool SampleRecorder::inputLooksDifferential() {
	return false;
}
#endif

Cluster* SampleCluster::getCluster(Sample* owner, uint32_t index, int32_t loadMode, uint32_t, Error* error) {
	if (error)
		*error = Error::NONE;
	if (recorder_test::failure == recorder_test::Failure::FIRST_CLUSTER || recorder_test::clusterLoadFails) {
		if (error)
			*error = Error::SD_CARD;
		return nullptr;
	}
	if (!cluster) {
		cluster = new Cluster();
		cluster->sample = owner;
		cluster->clusterIndex = index;
	}
	if (recorder_test::asyncClusterLoads && loadMode == CLUSTER_ENQUEUE && sdAddress)
		cluster->loaded = false;
	cluster->numReasonsToBeLoaded++;
	return cluster;
}
char const* inputChannelToString(AudioInputChannel) {
	return "Input";
}

extern "C" void freezeWithError(char const* code) {
	throw std::runtime_error(code);
}
extern "C" FRESULT f_unlink(TCHAR const*) {
	if (recorder_test::fileOpen)
		throw std::runtime_error("Unlink before close");
	recorder_test::unlinks++;
	return FR_OK;
}

extern "C" DRESULT disk_write(BYTE, const BYTE*, LBA_t, UINT) {
	recorder_test::headerWrites++;
	return recorder_test::headerFails ? RES_ERROR : RES_OK;
}

void* delugeAlloc(unsigned int size, bool) {
	return GeneralMemoryAllocator::get().allocStealable(size);
}
void SampleRecorder::totalSampleLengthNowKnown(uint32_t frames, uint32_t end) {
	sample->lengthInSamples = frames;
	sample->audioDataLengthBytes = frames * sample->byteDepth * sample->numChannels;
	sample->fileLoopEndSamples = end;
}
extern "C" LBA_t clst2sect(FATFS*, DWORD cluster) {
	return 32 + cluster * 64;
}
