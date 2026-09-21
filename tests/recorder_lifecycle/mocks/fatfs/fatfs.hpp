#pragma once
#include "fixture.h"
#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <vector>
extern "C" {
#include "fatfs/ff.h"
}
namespace FatFS {
struct OpenFile {
	OpenFile() { recorder_test::fileOpen = true; }
	~OpenFile() {
		recorder_test::fileCloses++;
		recorder_test::fileOpen = false;
		if (recorder_test::onClose)
			recorder_test::onClose();
	}
};
inline std::vector<std::byte> loopFileBytes;
struct File {
	FIL raw{};
	FIL& inner() { return raw; }
	std::expected<size_t, FRESULT> write(std::span<std::byte> bytes) {
		if (recorder_test::writeError != ::Error::NONE)
			return std::unexpected(FR_DISK_ERR);
		size_t end = recorder_test::filePosition + bytes.size();
		loopFileBytes.resize(std::max(loopFileBytes.size(), end));
		std::copy(bytes.begin(), bytes.end(), loopFileBytes.begin() + recorder_test::filePosition);
		recorder_test::filePosition = end;
		raw.fptr = end;
		raw.clust = (end - 1) / 32768;
		return bytes.size();
	}
	std::shared_ptr<OpenFile> handle = std::make_shared<OpenFile>();
	std::expected<void, FRESULT> close() {
		if (recorder_test::closeFails)
			return std::unexpected(FR_DISK_ERR);
		handle.reset();
		return {};
	}
	std::expected<void, FRESULT> lseek(uint32_t position) {
		recorder_test::seeks++;
		if (recorder_test::onSeek)
			recorder_test::onSeek();
		if (recorder_test::seekFails)
			return std::unexpected(FR_DISK_ERR);
		recorder_test::filePosition = position;
		raw.fptr = position;
		return {};
	}
	std::expected<void, FRESULT> sync() { return {}; }
	bool truncate() {
		recorder_test::truncations++;
		if (recorder_test::onTruncate)
			recorder_test::onTruncate();
		if (recorder_test::truncateFails)
			return false;
		recorder_test::fileSize = recorder_test::filePosition;
		return true;
	}
};
} // namespace FatFS
