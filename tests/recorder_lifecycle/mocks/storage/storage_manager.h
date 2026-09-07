#pragma once
#include "fatfs/fatfs.hpp"
#include <expected>
inline FATFS fileSystem{};
namespace StorageManager {
inline Error initSD() {
	return Error::NONE;
}
inline Error checkSpaceOnCard() {
	return Error::NONE;
}
inline std::expected<FatFS::File, Error> createFile(char const*, bool) {
	if (recorder_test::onCreate)
		recorder_test::onCreate();
	if (recorder_test::createError != Error::NONE)
		return std::unexpected(recorder_test::createError);
	return FatFS::File{};
}
} // namespace StorageManager
