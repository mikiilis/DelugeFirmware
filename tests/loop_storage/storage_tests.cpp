#include "CppUTest/TestHarness.h"
#include "fatfs/fatfs.hpp"
#include "loop_station/storage.h"
extern "C" {
#include "fatfs/diskio.h"
int pendingGlobalMIDICommandNumClustersWritten = 0;
}
#include <algorithm>
#include <cstring>
#include <vector>

namespace {
std::vector<BYTE> disk;
FATFS fs{};
bool failWrite = false;
bool failSync = false;
void put16(size_t at, uint16_t value) {
	disk[at] = value;
	disk[at + 1] = value >> 8;
}
void format() {
	// FAT16 superfloppy, 16 MiB, 2 KiB clusters, one FAT and 512 root entries.
	disk.assign(32768 * 512, 0);
	disk[0] = 0xeb;
	disk[1] = 0x3c;
	disk[2] = 0x90;
	put16(11, 512);
	disk[13] = 4;
	put16(14, 1);
	disk[16] = 1;
	put16(17, 512);
	put16(19, 32768);
	disk[21] = 0xf8;
	put16(22, 32);
	put16(510, 0xaa55);
	put16(512, 0xfff8);
	put16(514, 0xffff);
}
} // namespace
extern "C" {
DSTATUS disk_initialize(BYTE) {
	return 0;
}
DSTATUS disk_status(BYTE) {
	return 0;
}
DRESULT disk_read(BYTE, BYTE* data, LBA_t sector, UINT count) {
	if ((uint64_t(sector) + count) * 512 > disk.size())
		return RES_PARERR;
	std::memcpy(data, disk.data() + sector * 512, count * 512);
	return RES_OK;
}
DRESULT disk_write(BYTE, const BYTE* data, LBA_t sector, UINT count) {
	if (failWrite)
		return RES_ERROR;
	if ((uint64_t(sector) + count) * 512 > disk.size())
		return RES_PARERR;
	std::memcpy(disk.data() + sector * 512, data, count * 512);
	return RES_OK;
}
DRESULT disk_ioctl(BYTE, BYTE command, void*) {
	return failSync && command == CTRL_SYNC ? RES_ERROR : RES_OK;
}
DWORD get_fattime() {
	return (46u << 25) | (1u << 21) | (1u << 16);
}
}
TEST_GROUP(LoopStorage){void setup() override{failWrite = failSync = false;
format();
fs = {};
CHECK_EQUAL(FR_OK, f_mount(&fs, "", 1));
}
void teardown() override {
	failWrite = failSync = false;
	f_mount(nullptr, "", 0);
}
}
;
TEST(LoopStorage, RepeatedRewritesAndPartialTailRoundTrip) {
	auto opened = FatFS::File::open("loop.wav", FA_CREATE_ALWAYS | FA_WRITE);
	CHECK(opened.has_value());
	auto& file = *opened;
	const size_t block = fs.csize * 512;
	std::vector<std::byte> expected(block * 12 + 137);
	for (int pass = 0; pass < 3; ++pass) {
		for (size_t at = 0; at < expected.size(); at += block) {
			size_t count = std::min(block, expected.size() - at);
			std::fill_n(expected.begin() + at, count, std::byte(pass + at / block + 1));
			auto failure = deluge::loop_station::writeLoopSnapshot(file, at, std::span(expected).subspan(at, count),
			                                                       [] { return false; });
			CHECK(failure.stage == deluge::loop_station::StorageStage::NONE);
		}
	}
	CHECK(file.close().has_value());
	auto reader = FatFS::File::open("loop.wav", FA_READ);
	CHECK(reader.has_value());
	std::vector<std::byte> actual(expected.size());
	auto read = reader->read(actual);
	CHECK(read.has_value());
	CHECK_EQUAL(expected.size(), read->size());
	CHECK(actual == expected);
}

TEST(LoopStorage, PartialSectorMustBeSyncedBeforeRawClusterReload) {
	auto opened = FatFS::File::open("tail.wav", FA_CREATE_ALWAYS | FA_WRITE);
	CHECK(opened.has_value());
	std::vector<std::byte> data(137, std::byte{0x55});
	auto written = opened->write(data);
	CHECK(written.has_value());
	size_t address = (fs.database + (opened->inner().clust - 2) * fs.csize) * 512;
	CHECK(disk[address] != 0x55); // Successful write alone leaves data only in FIL.buf.
	auto failure = deluge::loop_station::writeLoopSnapshot(*opened, 0, data, [] { return false; });
	CHECK(failure.stage == deluge::loop_station::StorageStage::NONE);
	CHECK_EQUAL(0x55, disk[address]);
}
TEST(LoopStorage, DiskFailurePreservesStageAndCanRecordAfterCleanup) {
	auto opened = FatFS::File::open("failed.wav", FA_CREATE_ALWAYS | FA_WRITE);
	CHECK(opened.has_value());
	std::vector<std::byte> data(512, std::byte{0x37});
	failWrite = true;
	auto failure = deluge::loop_station::writeLoopSnapshot(*opened, 0, data, [] { return false; });
	CHECK(failure.stage == deluge::loop_station::StorageStage::WRITE);
	CHECK_EQUAL(FR_DISK_ERR, failure.rawResult);
	failWrite = false;
	opened->close();
	f_unlink("failed.wav");
	auto next = FatFS::File::open("retry.wav", FA_CREATE_ALWAYS | FA_WRITE);
	CHECK(next.has_value());
	failure = deluge::loop_station::writeLoopSnapshot(*next, 0, data, [] { return false; });
	CHECK(failure.stage == deluge::loop_station::StorageStage::NONE);
	CHECK(next->close().has_value());
}

TEST(LoopStorage, DiskFullSeekCannotSilentlyWriteAtWrongOffset) {
	auto opened = FatFS::File::open("full.wav", FA_CREATE_ALWAYS | FA_WRITE);
	CHECK(opened.has_value());
	std::vector<std::byte> data(512, std::byte{0x2a});
	auto failure = deluge::loop_station::writeLoopSnapshot(*opened, disk.size() + 512, data, [] { return false; });
	CHECK(failure.stage == deluge::loop_station::StorageStage::SHORT_SEEK);
	CHECK(failure.actual < failure.offset);
}
TEST(LoopStorage, PartialWriteSyncFailureIsDistinctFromWriteFailure) {
	auto opened = FatFS::File::open("sync.wav", FA_CREATE_ALWAYS | FA_WRITE);
	CHECK(opened.has_value());
	std::vector<std::byte> data(137, std::byte{0x55});
	// Allocate the cluster before injecting the failure, leaving only the
	// partial sector and directory/FAT synchronization for the tested write.
	CHECK(opened->write(data).has_value());
	failSync = true;
	auto failure = deluge::loop_station::writeLoopSnapshot(*opened, 0, data, [] { return false; });
	CHECK(failure.stage == deluge::loop_station::StorageStage::SYNC);
	CHECK_EQUAL(FR_DISK_ERR, failure.rawResult);
}

TEST(LoopStorage, DiskFullWriteReturnsActualShortCount) {
	auto opened = FatFS::File::open("full.wav", FA_CREATE_ALWAYS | FA_WRITE);
	CHECK(opened.has_value());
	std::vector<std::byte> data(disk.size(), std::byte{0x2a});
	auto failure = deluge::loop_station::writeLoopSnapshot(*opened, 0, data, [] { return false; });
	CHECK(failure.stage == deluge::loop_station::StorageStage::SHORT_WRITE);
	CHECK_EQUAL(FR_OK, failure.rawResult);
	CHECK(failure.actual > 0);
	CHECK(failure.actual < failure.requested);
}
