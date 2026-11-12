#include "CppUTest/TestHarness.h"
#include "fatfs/diskio.h"
#include "fatfs/ff.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <vector>
namespace {
std::vector<BYTE> disk;
unsigned reads = 0;
bool failWrites = false;
std::function<void()> onRead;
void fat(DWORD cluster, DWORD value) {
	for (unsigned i = 0; i < 4; ++i)
		disk[512 + cluster * 4 + i] = BYTE(value >> (i * 8));
}
} // namespace
extern "C" {
DWORD get_fat_from_fs(FATFS*, DWORD);
int pendingGlobalMIDICommandNumClustersWritten = 0;
DSTATUS disk_initialize(BYTE) {
	return 0;
}
DSTATUS disk_status(BYTE) {
	return 0;
}
DRESULT disk_read(BYTE, BYTE* buffer, LBA_t sector, UINT count) {
	++reads;
	if (onRead)
		onRead();
	if ((sector + count) * 512 > disk.size())
		return RES_ERROR;
	std::memcpy(buffer, disk.data() + sector * 512, count * 512);
	return RES_OK;
}
DRESULT disk_write(BYTE, const BYTE* buffer, LBA_t sector, UINT count) {
	if (failWrites)
		return RES_ERROR;
	if ((sector + count) * 512 > disk.size())
		return RES_ERROR;
	std::memcpy(disk.data() + sector * 512, buffer, count * 512);
	return RES_OK;
}
DRESULT disk_ioctl(BYTE, BYTE, void*) {
	return RES_OK;
}
DWORD get_fattime() {
	return 0;
}
}
TEST_GROUP(LoopFatFsAllocation) {
	FATFS fs{};
	FIL file{};
	FFArchiveWork work{};
	std::array<WCHAR, FF_MAX_LFN + 1> lfn{};
	void setup() override {
		// Firmware f_mount(opt=0) intentionally ignores replacement/unmount requests.
		f_mount(nullptr, "", 1);
		fs = {};
		file = {};
		work = {};
		disk.assign((64 + 4096 + 8) * 512, 0);
		reads = 0;
		failWrites = false;
		onRead = {};
		CHECK_EQUAL(FR_OK, f_mount(&fs, "", 0));
		fs.lfnbuf = lfn.data();
		fs.fs_type = FS_FAT32;
		fs.id = 77;
		fs.n_fats = 1;
		fs.csize = 1;
		fs.n_fatent = 4098;
		fs.fatbase = 1;
		fs.fsize = 33;
		fs.database = 64;
		fs.dirbase = 2;
		fs.free_clst = UINT32_MAX;
		fs.last_clst = 2;
		fs.winsect = UINT32_MAX;
		fs.fsi_flag = 0x80;
		for (DWORD i = 0; i < fs.n_fatent; ++i)
			fat(i, 0x0fffffff);
		file.obj.fs = &fs;
		file.obj.id = fs.id;
		file.flag = FA_WRITE;
	}
	void prepareFull128SectorRoot() {
		fs.csize = 128;
		fs.n_fatent = 130;
		disk.assign((64 + (fs.n_fatent - 2) * fs.csize + 8) * 512, 0);
		for (DWORD cluster = 0; cluster < fs.n_fatent; ++cluster)
			fat(cluster, 0x0fffffff);
		fat(fs.n_fatent - 1, 0);
		for (unsigned sector = 0; sector < fs.csize; ++sector) {
			for (unsigned entryIndex = 0; entryIndex < 16; ++entryIndex) {
				auto* entry = disk.data() + (fs.database + sector) * 512 + entryIndex * 32;
				std::fill_n(entry, 11, BYTE('A' + (sector + entryIndex) % 26));
				entry[11] = AM_ARC;
			}
		}
	}
	void teardown() override {
		onRead = {};
		failWrites = false;
		f_mount(nullptr, "", 1);
	}
};
TEST(LoopFatFsAllocation, unknownFullVolumeYieldsAndCancellationClearsCursorWithoutAllocating) {
	std::array<BYTE, 600> data{};
	UINT written = 0;
	bool canceled = false;
	onRead = [&] { canceled = true; };
	const auto result = f_write_archive(&file, data.data(), data.size(), &written, &work, 64);
	CHECK_EQUAL(FR_ARCHIVE_RETRY, result);
	CHECK(canceled);
	CHECK(work.examined <= 64);
	CHECK(reads <= 4);
	CHECK_EQUAL(0, written);
	CHECK_EQUAL(0, file.fptr);
	CHECK(work.valid);
	f_archive_work_reset(&work);
	CHECK_FALSE(work.valid);
	CHECK_EQUAL(0, work.directoryPhase);
	CHECK_FALSE(work.directoryValid);
	CHECK_EQUAL(0, work.next);
	onRead = {};
	reads = 0;
	CHECK_EQUAL(FR_OK, f_write(&file, data.data(), data.size(), &written));
	CHECK_EQUAL(0, written);
	CHECK(reads > 4); // Ordinary writers retain their existing API semantics.
}
TEST(LoopFatFsAllocation, fragmentedAllocationResumesAndPreservesPartialWriteOffsets) {
	fat(3000, 0);
	fat(3500, 0);
	std::array<BYTE, 600> data;
	for (unsigned i = 0; i < data.size(); ++i)
		data[i] = BYTE(i);
	UINT total = 0;
	bool partial = false;
	for (unsigned attempt = 0; attempt < 140 && total < data.size(); ++attempt) {
		UINT written = 0;
		reads = 0;
		const auto result = f_write_archive(&file, data.data() + total, data.size() - total, &written, &work, 64);
		CHECK(result == FR_ARCHIVE_RETRY || result == FR_OK);
		CHECK(work.examined <= 64);
		CHECK(reads <= 5);
		if (result == FR_ARCHIVE_RETRY && written)
			partial = true;
		total += written;
		CHECK_EQUAL(total, file.fptr);
	}
	CHECK_EQUAL(data.size(), total);
	CHECK(partial);
	CHECK_EQUAL(3500, file.clust);
	MEMCMP_EQUAL(data.data(), disk.data() + (64 + 3000 - 2) * 512, 512);
	MEMCMP_EQUAL(data.data() + 512, file.buf, 88);
}
TEST(LoopFatFsAllocation, rootDirectoryGrowthUsesTheSameBoundedSearchAndCanBeCanceled) {
	for (unsigned i = 0; i < 16; ++i) {
		auto* entry = disk.data() + fs.database * 512 + i * 32;
		std::fill_n(entry, 11, BYTE('A' + i));
		entry[11] = AM_ARC;
	}
	FIL candidate{};
	bool canceled = false;
	onRead = [&] { canceled = true; };
	const auto result = f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 64);
	CHECK_EQUAL(FR_ARCHIVE_RETRY, result);
	CHECK(canceled);
	CHECK(work.examined <= 64);
	CHECK(work.valid);
	CHECK_EQUAL(0, work.chain); // Root growth now reserves a detached cluster before clearing it.
	CHECK_EQUAL(0, candidate.obj.sclust);
	f_archive_work_reset(&work);
	CHECK_FALSE(work.valid);
}
TEST(LoopFatFsAllocation, ordinaryFatMutationInvalidatesAPausedSearchBeforeItCanReportFull) {
	fat(10, 0);
	FIL old{};
	std::array<BYTE, 600> data{};
	UINT written = 0;
	CHECK_EQUAL(FR_OK, f_open(&old, "OLD.WAV", FA_WRITE | FA_CREATE_NEW));
	CHECK_EQUAL(FR_OK, f_write(&old, data.data(), 512, &written));
	CHECK_EQUAL(512, written);
	CHECK_EQUAL(10, old.obj.sclust);
	CHECK_EQUAL(FR_OK, f_close(&old));
	fs.last_clst = 2;
	CHECK_EQUAL(FR_ARCHIVE_RETRY, f_write_archive(&file, data.data(), data.size(), &written, &work, 64));
	const auto revision = fs.archive_revision;
	CHECK_EQUAL(0x0fffffff, get_fat_from_fs(&fs, 10));
	CHECK_EQUAL(FR_OK, f_unlink("OLD.WAV"));
	CHECK_EQUAL(0, get_fat_from_fs(&fs, 10));
	CHECK(fs.archive_revision != revision);
	CHECK_EQUAL(FR_ARCHIVE_RETRY, f_write_archive(&file, data.data(), data.size(), &written, &work, 64));
	CHECK_EQUAL(512, written);
	CHECK_EQUAL(10, file.obj.sclust);
}
TEST(LoopFatFsAllocation, fragmentedCandidateRemovalFreesAtMostItsClusterBudgetPerCall) {
	constexpr unsigned count = 300;
	for (unsigned i = 0; i < count; ++i)
		fat(10 + i * 10, i + 1 < count ? 20 + i * 10 : 0x0fffffff);
	auto* entry = disk.data() + fs.database * 512;
	std::memcpy(entry, "DELETE  WAV", 11);
	entry[11] = AM_ARC;
	entry[26] = 10;
	const DWORD size = count * 512;
	for (unsigned i = 0; i < 4; ++i)
		entry[28 + i] = BYTE(size >> (8 * i));
	FFArchiveDelete removal{};
	FRESULT result = FR_ARCHIVE_RETRY;
	unsigned calls = 0;
	while (result == FR_ARCHIVE_RETRY && calls++ < 30) {
		result = f_unlink_archive("DELETE.WAV", &removal, 16);
		CHECK(removal.examined <= 16);
	}
	CHECK_EQUAL(FR_OK, result);
	CHECK(calls > count / 16);
	for (unsigned i = 0; i < count; ++i) {
		const auto* bytes = disk.data() + 512 + (10 + i * 10) * 4;
		CHECK_EQUAL(0, bytes[0] | bytes[1] | bytes[2] | bytes[3]);
	}
	CHECK_EQUAL(0xE5, disk[fs.database * 512]);
}
TEST(LoopFatFsAllocation, rootDirectoryGrowthResumesUntilAFragmentedFreeClusterIsFound) {
	for (unsigned i = 0; i < 16; ++i) {
		auto* entry = disk.data() + fs.database * 512 + i * 32;
		std::fill_n(entry, 11, BYTE('A' + i));
		entry[11] = AM_ARC;
	}
	fat(3000, 0);
	FIL candidate{};
	FRESULT result = FR_ARCHIVE_RETRY;
	unsigned calls = 0;
	while (result == FR_ARCHIVE_RETRY && calls++ < 80) {
		result = f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 64);
		CHECK(work.examined <= 64);
	}
	CHECK_EQUAL(FR_OK, result);
	CHECK(calls > 1);
	CHECK_EQUAL(3000, get_fat_from_fs(&fs, 2));
	CHECK_EQUAL(FR_OK, f_close(&candidate));
}
TEST(LoopFatFsAllocation, largeRootLookupAndAllocationResumeWithinEntryBudget) {
	fat(2, 3);
	fat(3, 0x0fffffff);
	for (unsigned i = 0; i < 16; ++i) {
		auto* entry = disk.data() + fs.database * 512 + i * 32;
		std::fill_n(entry, 11, BYTE('A' + i));
		entry[11] = AM_ARC;
	}
	for (unsigned i = 0; i < 8; ++i) {
		auto* entry = disk.data() + (fs.database + 1) * 512 + i * 32;
		std::fill_n(entry, 11, BYTE('a' + i));
		entry[11] = AM_ARC;
	}
	FIL candidate{};
	FRESULT result = FR_ARCHIVE_RETRY;
	unsigned calls = 0;
	while (result == FR_ARCHIVE_RETRY && calls++ < 20) {
		result = f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 8);
		CHECK(work.directoryExamined <= 8);
		CHECK(work.examined <= 8);
	}
	CHECK_EQUAL(FR_OK, result);
	CHECK(calls >= 6);
	CHECK_EQUAL(3, get_fat_from_fs(&fs, 2));
	CHECK_EQUAL(FR_OK, f_close(&candidate));
}
TEST(LoopFatFsAllocation, directoryMutationRestartsPausedLookupBeforeCreatingDuplicate) {
	fat(2, 3);
	fat(3, 0x0fffffff);
	fat(3000, 0);
	for (unsigned cluster = 0; cluster < 2; ++cluster) {
		for (unsigned i = 0; i < 16; ++i) {
			auto* entry = disk.data() + (fs.database + cluster) * 512 + i * 32;
			std::fill_n(entry, 11, BYTE('A' + i));
			entry[11] = AM_ARC;
		}
	}
	FIL candidate{};
	CHECK_EQUAL(FR_ARCHIVE_RETRY, f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 8));
	CHECK(work.directoryPhase == 1);
	FIL ordinary{};
	CHECK_EQUAL(FR_OK, f_open(&ordinary, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW));
	CHECK_EQUAL(FR_OK, f_close(&ordinary));
	FRESULT result = FR_ARCHIVE_RETRY;
	unsigned calls = 0;
	while (result == FR_ARCHIVE_RETRY && calls++ < 20) {
		result = f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 8);
		CHECK(work.directoryExamined <= 8);
	}
	CHECK_EQUAL(FR_EXIST, result);
}
TEST(LoopFatFsAllocation, fullRootAndFullVolumeYieldUntilCreationIsDenied) {
	for (unsigned i = 0; i < 16; ++i) {
		auto* entry = disk.data() + fs.database * 512 + i * 32;
		std::fill_n(entry, 11, BYTE('A' + i));
		entry[11] = AM_ARC;
	}
	FIL candidate{};
	FRESULT result = FR_ARCHIVE_RETRY;
	unsigned calls = 0;
	while (result == FR_ARCHIVE_RETRY && calls++ < 600) {
		result = f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 8);
		CHECK(work.directoryExamined <= 8);
		CHECK(work.examined <= 8);
	}
	CHECK_EQUAL(FR_DENIED, result);
	CHECK(calls > 400);
	CHECK_EQUAL(0, fs.free_clst);
}
TEST(LoopFatFsAllocation, largeRootClusterClearingIsBudgetedAndLinksOnlyAfterComplete) {
	fs.csize = 8;
	fs.n_fatent = 16;
	fat(2, 0x0fffffff);
	for (DWORD cluster = 3; cluster < 15; ++cluster)
		fat(cluster, 0x0fffffff);
	fat(15, 0);
	for (unsigned sector = 0; sector < fs.csize; ++sector) {
		for (unsigned entryIndex = 0; entryIndex < 16; ++entryIndex) {
			auto* entry = disk.data() + (fs.database + sector) * 512 + entryIndex * 32;
			std::fill_n(entry, 11, BYTE('A' + (sector + entryIndex) % 26));
			entry[11] = AM_ARC;
		}
	}
	FIL candidate{};
	FRESULT result = FR_ARCHIVE_RETRY;
	unsigned calls = 0;
	unsigned clearCalls = 0;
	DWORD reserved = 0;
	while (result == FR_ARCHIVE_RETRY && calls++ < 100) {
		result = f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 3);
		CHECK(work.directoryExamined <= 3);
		CHECK(work.examined <= 3);
		CHECK(work.clearExamined <= 3);
		if (work.clearPhase == 1) {
			++clearCalls;
			reserved = work.clearCluster;
			CHECK_EQUAL(0x0fffffff, get_fat_from_fs(&fs, 2));
			CHECK(get_fat_from_fs(&fs, reserved) >= fs.n_fatent);
		}
	}
	CHECK_EQUAL(FR_OK, result);
	CHECK(calls > 80);
	CHECK(clearCalls >= 2); // The completing call clears the final two sectors and links the cluster.
	CHECK_EQUAL(reserved, get_fat_from_fs(&fs, 2));
	CHECK(get_fat_from_fs(&fs, reserved) >= fs.n_fatent);
	for (unsigned sector = 0; sector < fs.csize; ++sector) {
		const auto* bytes = disk.data() + (fs.database + (reserved - 2) * fs.csize + sector) * 512;
		const auto begin = sector == 0 ? bytes + 32 : bytes;
		CHECK(std::all_of(begin, bytes + 512, [](BYTE value) { return value == 0; }));
	}
	CHECK_EQUAL(FR_OK, f_close(&candidate));
}
TEST(LoopFatFsAllocation, cancellationFreesUnlinkedPartiallyClearedClusterAndRetainsCleanupPhase) {
	fs.csize = 8;
	fs.n_fatent = 16;
	fat(2, 0x0fffffff);
	for (DWORD cluster = 3; cluster < 15; ++cluster)
		fat(cluster, 0x0fffffff);
	fat(15, 0);
	for (unsigned sector = 0; sector < fs.csize; ++sector) {
		for (unsigned entryIndex = 0; entryIndex < 16; ++entryIndex) {
			auto* entry = disk.data() + (fs.database + sector) * 512 + entryIndex * 32;
			std::fill_n(entry, 11, BYTE('A' + (sector + entryIndex) % 26));
			entry[11] = AM_ARC;
		}
	}
	FIL candidate{};
	FRESULT result = FR_ARCHIVE_RETRY;
	unsigned calls = 0;
	while (result == FR_ARCHIVE_RETRY && work.clearPhase != 1 && calls++ < 100) {
		result = f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 3);
		CHECK(work.clearExamined <= 3);
	}
	CHECK_EQUAL(1, work.clearPhase);
	const DWORD reserved = work.clearCluster;
	CHECK(reserved >= 2);
	CHECK_EQUAL(0x0fffffff, get_fat_from_fs(&fs, 2));
	CHECK(get_fat_from_fs(&fs, reserved) >= fs.n_fatent);
	f_archive_work_reset(&work);
	CHECK_EQUAL(2, work.clearPhase);
	CHECK_EQUAL(FR_ARCHIVE_RETRY, f_archive_work_cleanup(&work, 3));
	CHECK_EQUAL(0, get_fat_from_fs(&fs, reserved));
	CHECK_EQUAL(0x0fffffff, get_fat_from_fs(&fs, 2));
	CHECK_EQUAL(3, work.clearPhase);
	failWrites = true;
	CHECK_EQUAL(FR_DISK_ERR, f_archive_work_cleanup(&work, 3));
	CHECK_EQUAL(3, work.clearPhase);
	CHECK_EQUAL(0x0fffffff, get_fat_from_fs(&fs, 2));
	failWrites = false;
	CHECK_EQUAL(FR_OK, f_archive_work_cleanup(&work, 3));
	CHECK_EQUAL(0, work.clearPhase);
	CHECK_EQUAL(0x0fffffff, get_fat_from_fs(&fs, 2));
}
TEST(LoopFatFsAllocation, productionBudgetStillClearsLargeRootClusterInFourSectorSlices) {
	prepareFull128SectorRoot();
	FIL candidate{};
	FRESULT result = FR_ARCHIVE_RETRY;
	unsigned calls = 0;
	unsigned clearCalls = 0;
	DWORD reserved = 0;
	while (result == FR_ARCHIVE_RETRY && calls++ < 100) {
		result = f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 128);
		CHECK(work.clearExamined <= 4);
		if (work.clearPhase == 1) {
			++clearCalls;
			reserved = work.clearCluster;
			CHECK_EQUAL(0x0fffffff, get_fat_from_fs(&fs, 2));
			CHECK(get_fat_from_fs(&fs, reserved) >= fs.n_fatent);
		}
		else if (reserved && result == FR_OK) {
			for (unsigned sector = 0; sector < fs.csize; ++sector) {
				const auto* bytes = disk.data() + (fs.database + (reserved - 2) * fs.csize + sector) * 512;
				const auto begin = sector == 0 ? bytes + 32 : bytes;
				CHECK(std::all_of(begin, bytes + 512, [](BYTE value) { return value == 0; }));
			}
		}
	}
	CHECK_EQUAL(FR_OK, result);
	CHECK(calls > 32);
	CHECK_EQUAL(31, clearCalls); // The 32nd slice completes and clears the phase before return.
	CHECK_EQUAL(reserved, get_fat_from_fs(&fs, 2));
	CHECK(get_fat_from_fs(&fs, reserved) >= fs.n_fatent);
	CHECK_EQUAL(FR_OK, f_close(&candidate));
}
TEST(LoopFatFsAllocation, cancelLargeRootClearCleansDetachedClusterWithoutLinking) {
	prepareFull128SectorRoot();
	FIL candidate{};
	FRESULT result = FR_ARCHIVE_RETRY;
	unsigned calls = 0;
	while (result == FR_ARCHIVE_RETRY && work.clearPhase != 1 && calls++ < 100) {
		result = f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 128);
		CHECK(work.clearExamined <= 4);
	}
	CHECK_EQUAL(1, work.clearPhase);
	CHECK_EQUAL(4, work.clearExamined);
	const DWORD reserved = work.clearCluster;
	CHECK_EQUAL(0x0fffffff, get_fat_from_fs(&fs, 2));
	CHECK(get_fat_from_fs(&fs, reserved) >= fs.n_fatent);
	f_archive_work_reset(&work);
	CHECK_EQUAL(2, work.clearPhase);
	CHECK_EQUAL(FR_ARCHIVE_RETRY, f_archive_work_cleanup(&work, 128));
	CHECK_EQUAL(0, get_fat_from_fs(&fs, reserved));
	CHECK_EQUAL(0x0fffffff, get_fat_from_fs(&fs, 2));
	CHECK_EQUAL(3, work.clearPhase);
	CHECK_EQUAL(FR_OK, f_archive_work_cleanup(&work, 128));
	CHECK_EQUAL(0, work.clearPhase);
	CHECK_EQUAL(0x0fffffff, get_fat_from_fs(&fs, 2));
}
TEST(LoopFatFsAllocation, directoryMutationDuringClearDiscardsDetachedClusterAndRestartsLookup) {
	fs.csize = 8;
	fs.n_fatent = 16;
	fat(2, 0x0fffffff);
	for (DWORD cluster = 3; cluster < 15; ++cluster)
		fat(cluster, 0x0fffffff);
	fat(15, 0);
	for (unsigned sector = 0; sector < fs.csize; ++sector) {
		for (unsigned entryIndex = 0; entryIndex < 16; ++entryIndex) {
			auto* entry = disk.data() + (fs.database + sector) * 512 + entryIndex * 32;
			std::fill_n(entry, 11, BYTE('A' + (sector + entryIndex) % 26));
			entry[11] = AM_ARC;
		}
	}
	auto* first = disk.data() + fs.database * 512;
	std::memcpy(first, "OLD     WAV", 11);
	first[11] = AM_ARC;
	FIL candidate{};
	FRESULT result = FR_ARCHIVE_RETRY;
	unsigned calls = 0;
	while (result == FR_ARCHIVE_RETRY && work.clearPhase != 1 && calls++ < 100) {
		result = f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 3);
		CHECK(work.directoryExamined <= 3);
		CHECK(work.clearExamined <= 3);
	}
	CHECK_EQUAL(1, work.clearPhase);
	const DWORD reserved = work.clearCluster;
	CHECK_EQUAL(FR_OK, f_unlink("OLD.WAV"));
	result = FR_ARCHIVE_RETRY;
	calls = 0;
	while (result == FR_ARCHIVE_RETRY && calls++ < 100) {
		result = f_open_archive(&candidate, "L1A23BC0.WAV", FA_WRITE | FA_CREATE_NEW, &work, 3);
		CHECK(work.directoryExamined <= 3);
		CHECK(work.clearExamined <= 3);
	}
	CHECK_EQUAL(FR_OK, result);
	CHECK_EQUAL(0x0fffffff, get_fat_from_fs(&fs, 2));
	CHECK_EQUAL(0, get_fat_from_fs(&fs, reserved));
	CHECK_EQUAL('L', first[0]);
	CHECK_EQUAL('1', first[1]);
	CHECK_EQUAL(FR_OK, f_close(&candidate));
}
TEST(LoopFatFsAllocation, unknownFullVolumeEventuallyReportsFullAfterBoundedRetries) {
	std::array<BYTE, 600> data{};
	FRESULT result = FR_ARCHIVE_RETRY;
	unsigned calls = 0;
	while (result == FR_ARCHIVE_RETRY && calls++ < 100) {
		UINT written = 0;
		result = f_write_archive(&file, data.data(), data.size(), &written, &work, 64);
		CHECK(work.examined <= 64);
		CHECK_EQUAL(0, written);
	}
	CHECK_EQUAL(FR_OK, result);
	CHECK_EQUAL(64, calls);
	CHECK_EQUAL(0, fs.free_clst);
	CHECK_FALSE(work.valid);
}
