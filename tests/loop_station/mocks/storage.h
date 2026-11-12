#pragma once
#include "fatfs/ff.h"
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>
namespace archive_mock {
struct Storage {
	std::map<std::string, std::vector<std::byte>> files;
	std::map<FIL*, std::string> open;
	std::function<void(const char*)> yielding;
	std::string failOperation;
	std::map<std::string, unsigned> calls;
	UINT largestReadRequest = 0;
	std::vector<std::string> operations;
	FATFS filesystem{};
	std::map<unsigned, UINT> retryWrites;
	unsigned workResets = 0;
	DWORD freeClusters = 1;
	FRESULT freeResult = FR_OK;
	FRESULT failure = FR_DISK_ERR;
	unsigned failAfter = 0, matchingCalls = 0;
	bool shortWrite = false, shortSeek = false, shortRead = false;
	FRESULT call(const char* operation);
};
extern Storage storage;
} // namespace archive_mock
