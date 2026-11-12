#include "storage.h"
#include <algorithm>
#include <cstring>
namespace archive_mock {
Storage storage;
FRESULT Storage::call(const char* operation) {
	++calls[operation];
	operations.emplace_back(operation);
	if (yielding)
		yielding(operation);
	if (failOperation == operation && matchingCalls++ == failAfter)
		return failure;
	return FR_OK;
}
} // namespace archive_mock
using archive_mock::storage;
extern "C" {
FRESULT f_getfree(const TCHAR*, DWORD* clusters, FATFS** fs) {
	const auto result = storage.call("getfree");
	if (result != FR_OK)
		return result;
	static FATFS filesystem;
	*fs = &filesystem;
	*clusters = storage.freeClusters;
	return storage.freeResult;
}
void f_archive_work_reset(FFArchiveWork* work) {
	++storage.workResets;
	std::memset(work, 0, sizeof(*work));
}
FRESULT f_archive_work_cleanup(FFArchiveWork*, UINT) {
	return FR_OK;
}
FRESULT f_open_archive(FIL* file, const TCHAR* path, BYTE mode, FFArchiveWork* work, UINT) {
	const auto result = f_open(file, path, mode);
	if (result == FR_ARCHIVE_RETRY) {
		work->valid = 1;
		work->next = 123;
	}
	else
		work->valid = 0;
	return result;
}
FRESULT f_write_archive(FIL* file, const void* data, UINT count, UINT* written, FFArchiveWork* work, UINT) {
	const auto result = f_write(file, data, count, written);
	if (result == FR_ARCHIVE_RETRY) {
		work->valid = 1;
		work->next = 123;
	}
	else
		work->valid = 0;
	return result;
}
FRESULT f_unlink_archive(const TCHAR* path, FFArchiveDelete*, UINT) {
	return f_unlink(path);
}
FRESULT f_mkdir(const TCHAR*) {
	return storage.call("mkdir");
}
FRESULT f_open(FIL* file, const TCHAR* path, BYTE mode) {
	const auto result = storage.call("open");
	if (result != FR_OK)
		return result;
	if ((mode & FA_CREATE_NEW) && storage.files.contains(path))
		return FR_EXIST;
	if (mode & FA_READ) {
		const auto existing = storage.files.find(path);
		if (existing == storage.files.end())
			return FR_NO_FILE;
		file->obj.objsize = existing->second.size();
	}
	else
		storage.files[path] = {};
	storage.open[file] = path;
	file->fptr = 0;
	return FR_OK;
}
FRESULT f_read(FIL* file, void* data, UINT count, UINT* read) {
	storage.largestReadRequest = std::max(storage.largestReadRequest, count);
	const auto result = storage.call("read");
	*read = 0;
	if (result != FR_OK)
		return result;
	const auto it = storage.open.find(file);
	if (it == storage.open.end())
		return FR_INVALID_OBJECT;
	const auto& bytes = storage.files[it->second];
	*read = static_cast<UINT>(std::min<size_t>(count, bytes.size() > file->fptr ? bytes.size() - file->fptr : 0));
	if (storage.shortRead && file->fptr >= 44 && *read >= 6)
		*read -= 6;
	if (*read)
		std::memcpy(data, bytes.data() + file->fptr, *read);
	file->fptr += *read;
	return FR_OK;
}
FRESULT f_write(FIL* file, const void* data, UINT count, UINT* written) {
	auto result = storage.call("write");
	*written = 0;
	if (result != FR_OK && result != FR_ARCHIVE_RETRY)
		return result;
	const auto it = storage.open.find(file);
	if (it == storage.open.end())
		return FR_INVALID_OBJECT;
	*written = count - (storage.shortWrite && count ? 1 : 0);
	if (const auto retry = storage.retryWrites.find(storage.calls["write"]); retry != storage.retryWrites.end()) {
		*written = std::min(count, retry->second);
		result = FR_ARCHIVE_RETRY;
	}
	else if (result == FR_ARCHIVE_RETRY)
		*written = 0;
	auto& bytes = storage.files[it->second];
	bytes.resize(std::max<size_t>(bytes.size(), file->fptr + *written));
	std::memcpy(bytes.data() + file->fptr, data, *written);
	file->fptr += *written;
	return result;
}
FRESULT f_lseek(FIL* file, FSIZE_t offset) {
	const auto result = storage.call("seek");
	if (result == FR_OK)
		file->fptr = offset + storage.shortSeek;
	return result;
}
FRESULT f_sync(FIL*) {
	return storage.call("sync");
}
FRESULT f_close(FIL* file) {
	const auto result = storage.call("close");
	if (result == FR_OK)
		storage.open.erase(file);
	return result;
}
FRESULT f_unlink(const TCHAR* path) {
	const auto result = storage.call("unlink");
	if (result == FR_OK)
		storage.files.erase(path);
	return result;
}
}
