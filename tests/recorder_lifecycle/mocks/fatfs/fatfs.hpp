#pragma once
#include "fixture.h"
#include <memory>
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
struct File {
	std::shared_ptr<OpenFile> handle = std::make_shared<OpenFile>();
	bool close() {
		if (recorder_test::closeFails)
			return false;
		handle.reset();
		return true;
	}
	bool lseek(uint32_t position) {
		recorder_test::seeks++;
		if (recorder_test::onSeek)
			recorder_test::onSeek();
		if (recorder_test::seekFails)
			return false;
		recorder_test::filePosition = position;
		return true;
	}
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
