#include "archive_storage.h"
#include <cstring>
namespace loop_station {
namespace {
bool validPublishedName(SlotId slot, const char* path) {
	if (!validSlot(slot) || !path || std::strlen(path) != 12 || path[0] != 'L'
	    || path[1] != static_cast<char>('1' + slot) || std::memcmp(path + 8, ".WAV", 4))
		return false;
	for (unsigned i = 2; i < 8; ++i)
		if (!((path[i] >= '0' && path[i] <= '9') || (path[i] >= 'A' && path[i] <= 'F')))
			return false;
	return true;
}
} // namespace
Status FatFsArchiveStorage::fail(FRESULT result, bool full) {
	if (error_ == MediaError::None) {
		firstError_ = result;
		error_ = full                                                                               ? MediaError::Full
		         : result == FR_NOT_READY || result == FR_INVALID_DRIVE || result == FR_NOT_ENABLED ? MediaError::Absent
		         : result == FR_WRITE_PROTECTED ? MediaError::Protected
		                                        : MediaError::Io;
	}
	return Status::StorageError;
}
Status FatFsArchiveStorage::failCreation(FRESULT result) {
	// Never scan the FAT for diagnostics. Unknown or unmounted cache stays Io.
	const bool known = filesystem_ && filesystem_->fs_type && filesystem_->n_fatent >= 2
	                   && filesystem_->free_clst <= filesystem_->n_fatent - 2;
	beginning_ = false;
	return fail(result, result == FR_DENIED && known && filesystem_->free_clst == 0);
}
const char* FatFsArchiveStorage::candidatePath(SlotId slot, Generation identity) const {
	if (!validSlot(slot))
		return nullptr;
	if (files_[slot].identity == identity)
		return files_[slot].path;
	const auto* published = publishedFile(slot, identity);
	return published ? published->path : nullptr;
}
const FatFsArchiveStorage::PublishedFile* FatFsArchiveStorage::publishedFile(SlotId slot, Generation identity) const {
	for (const auto& file : published_)
		if (file.slot == slot && file.identity == identity)
			return &file;
	return nullptr;
}
Status FatFsArchiveStorage::registerPublishedFile(SlotId slot, Generation identity, const char* path) {
	if (!identity || !validPublishedName(slot, path))
		return Status::Invalid;
	PublishedFile* available = nullptr;
	for (auto& file : published_) {
		if (file.slot == slot && file.identity == identity)
			return std::strcmp(file.path, path) == 0 ? Status::Ok : Status::Invalid;
		if (file.identity && std::strcmp(file.path, path) == 0)
			return Status::Invalid;
		if (!file.identity && !available)
			available = &file;
	}
	if (!available)
		return Status::NoMemory;
	available->slot = slot;
	available->identity = identity;
	std::strncpy(available->path, path, sizeof(available->path) - 1);
	return Status::Ok;
}
Status FatFsArchiveStorage::begin(SlotId slot, Generation identity) {
	if (!validSlot(slot) || !identity || open_)
		return Status::Invalid;
	if (files_[slot].transferred)
		return Status::NoMemory; // The bounded published-path table is full.
	if (!beginning_) {
		if (work_.clearPhase) {
			const auto cleanup = f_archive_work_cleanup(&work_, 128);
			if (cleanup != FR_OK && cleanup != FR_ARCHIVE_RETRY)
				return failCreation(cleanup);
			return Status::Busy;
		}
		error_ = MediaError::None;
		firstError_ = FR_OK;
		bytesWritten_ = 0;
		slot_ = slot;
		identity_ = identity;
		owns_ = haveHeader_ = false;
		collision_ = finalizeStep_ = rewriteWritten_ = 0;
		f_archive_work_reset(&work_);
		files_[slot].identity = 0;
		files_[slot].transferred = false;
		beginning_ = true;
	}
	else if (slot_ != slot || identity_ != identity)
		return Status::Invalid;
	auto& target = files_[slot];
	constexpr char prefix[] = "L";
	constexpr char hex[] = "0123456789ABCDEF";
	char* path = target.path;
	std::memcpy(path, prefix, sizeof(prefix) - 1);
	path += sizeof(prefix) - 1;
	*path++ = '1' + slot;
	for (unsigned digit = 0; digit < 4; ++digit)
		*path++ = hex[(identity >> ((3 - digit) * 4)) & 15];
	*path++ = hex[collision_ >> 4];
	*path++ = hex[collision_ & 15];
	std::memcpy(path, ".WAV", 5);
	const auto result = f_open_archive(&file_, target.path, FA_WRITE | FA_CREATE_NEW, &work_, 128);
	if (result == FR_ARCHIVE_RETRY)
		return Status::Busy;
	if (result == FR_EXIST && ++collision_ < 256)
		return Status::Busy;
	if (result != FR_OK)
		return failCreation(result);
	beginning_ = false;
	target.identity = identity;
	open_ = owns_ = true;
	return Status::Ok;
}
Status FatFsArchiveStorage::write(std::span<const std::byte> bytes, size_t& written) {
	written = 0;
	if (!open_ || bytes.size() > UINT32_MAX)
		return Status::Invalid;
	if (!haveHeader_) {
		if (bytes.size() != header_.size())
			return Status::Invalid;
		std::memcpy(header_.data(), bytes.data(), header_.size());
		haveHeader_ = true;
	}
	UINT actual = 0;
	const auto result = f_write_archive(&file_, bytes.data(), bytes.size(), &actual, &work_, 128);
	written = actual;
	bytesWritten_ += actual;
	return result == FR_ARCHIVE_RETRY ? Status::Busy
	       : result != FR_OK          ? fail(result)
	       : actual != bytes.size()   ? fail(FR_OK, true)
	                                  : Status::Ok;
}
Status FatFsArchiveStorage::finalize() {
	if (!open_ || !haveHeader_)
		return Status::Invalid;
	switch (finalizeStep_) {
	case 0: {
		const auto result = f_lseek(&file_, 0);
		if (result != FR_OK || f_tell(&file_) != 0)
			return fail(result == FR_OK ? FR_INT_ERR : result);
		++finalizeStep_;
		return Status::Busy;
	}
	case 1: {
		UINT written = 0;
		const auto result = f_write_archive(&file_, header_.data() + rewriteWritten_, header_.size() - rewriteWritten_,
		                                    &written, &work_, 128);
		bytesWritten_ += written;
		const auto remaining = header_.size() - rewriteWritten_;
		if (written > remaining)
			return fail(FR_INT_ERR);
		rewriteWritten_ += written;
		if (result == FR_ARCHIVE_RETRY)
			return Status::Busy;
		if (result != FR_OK || written != remaining)
			return fail(result, result == FR_OK);
		++finalizeStep_;
		return Status::Busy;
	}
	case 2: {
		const auto result = f_sync(&file_);
		if (result != FR_OK)
			return fail(result);
		++finalizeStep_;
		return Status::Busy;
	}
	case 3: {
		const auto result = f_close(&file_);
		if (result != FR_OK)
			return fail(result);
		open_ = false;
		++finalizeStep_;
		return Status::Ok;
	}
	default:
		return Status::Invalid;
	}
}
void FatFsArchiveStorage::abort() {
	// Request only: no FatFs call may hide inside a failed operation's service.
	beginning_ = false;
	f_archive_work_reset(&work_);
	if (validSlot(slot_))
		cleanup_[slot_] = {};
}
Status FatFsArchiveStorage::serviceAbort() {
	const auto allocationCleanup = f_archive_work_cleanup(&work_, 128);
	if (allocationCleanup == FR_ARCHIVE_RETRY)
		return Status::Busy;
	if (allocationCleanup != FR_OK) {
		cleanupError(slot_, allocationCleanup);
		return Status::StorageError;
	}
	if (open_) {
		const auto result = f_close(&file_);
		open_ = false;
		if (result != FR_OK)
			cleanupError(slot_, result);
		return owns_ ? Status::Busy : result == FR_OK ? Status::Ok : Status::StorageError;
	}
	if (owns_) {
		if (!files_[slot_].deleting)
			requestDiscard(slot_, identity_, false);
		const auto status = serviceDiscard(slot_, identity_);
		if (status != Status::Busy)
			owns_ = false;
		return status;
	}
	return validSlot(slot_) && cleanup_[slot_].error != MediaError::None ? Status::StorageError : Status::Ok;
}
void FatFsArchiveStorage::transfer(SlotId slot, Generation identity) {
	if (validSlot(slot) && files_[slot].identity == identity) {
		PublishedFile* destination = nullptr;
		for (auto& file : published_) {
			if (file.slot == slot && file.identity == identity) {
				destination = &file;
				break;
			}
			if (!file.identity && !destination)
				destination = &file;
		}
		if (destination) {
			destination->slot = slot;
			destination->identity = identity;
			std::memcpy(destination->path, files_[slot].path, sizeof(destination->path));
			files_[slot].identity = 0;
			files_[slot].transferred = false;
		}
		else
			files_[slot].transferred = true;
	}
	if (slot_ == slot && identity_ == identity)
		owns_ = false;
}

Status FatFsArchiveStorage::readCheckpoint(SlotId slot, Generation identity, FrameIndex first, std::span<Frame> frames,
                                           size_t& framesRead, FrameIndex& totalFrames) {
	framesRead = 0;
	totalFrames = 0;
	if (!validSlot(slot) || !identity || frames.size() > 512)
		return Status::Invalid;
	if (readerStep_ == ReaderStep::Idle) {
		const auto* published = publishedFile(slot, identity);
		const char* path = published                                                       ? published->path
		                   : files_[slot].identity == identity && files_[slot].transferred ? files_[slot].path
		                                                                                   : nullptr;
		if (!path || (files_[slot].identity == identity && files_[slot].deleting))
			return Status::SourceUnavailable;
		std::memcpy(readPath_.data(), path, readPath_.size());
		readSlot_ = slot;
		readIdentity_ = identity;
		readNextFrame_ = readTotalFrames_ = 0;
		readerCancel_ = false;
		readerOpen_ = false;
		f_archive_work_reset(&readWork_);
		readerStep_ = ReaderStep::Open;
		return Status::Busy;
	}
	if (readerStep_ == ReaderStep::Closing || readerCancel_) {
		return readerCall_ ? Status::Busy : Status::Stale;
	}
	if (readSlot_ != slot || readIdentity_ != identity)
		return Status::Busy;
	if (readerStep_ == ReaderStep::Open) {
		readerCall_ = true;
		const auto result = f_open_archive(&reader_, readPath_.data(), FA_READ, &readWork_, 128);
		readerCall_ = false;
		if (readerCancel_) {
			if (result == FR_OK)
				readerOpen_ = true;
			readerStep_ = ReaderStep::Closing;
			return Status::Stale;
		}
		if (result == FR_ARCHIVE_RETRY)
			return Status::Busy;
		if (result != FR_OK) {
			readerStep_ = ReaderStep::Idle;
			readSlot_ = kInvalidSlot;
			readIdentity_ = 0;
			return result == FR_NO_FILE || result == FR_NO_PATH ? Status::SourceUnavailable : Status::StorageError;
		}
		readerOpen_ = true;
		if (readerCancel_) {
			readerStep_ = ReaderStep::Closing;
			return Status::Stale;
		}
		readerStep_ = ReaderStep::Header;
		return Status::Busy;
	}
	if (readerStep_ == ReaderStep::Header) {
		UINT actual = 0;
		readerCall_ = true;
		const auto result = f_read(&reader_, readHeader_.data(), readHeader_.size(), &actual);
		readerCall_ = false;
		if (readerCancel_) {
			readerStep_ = ReaderStep::Closing;
			return Status::Stale;
		}
		auto u8 = [&](unsigned i) { return std::to_integer<uint8_t>(readHeader_[i]); };
		auto u16 = [&](unsigned i) { return uint16_t(u8(i) | (uint16_t(u8(i + 1)) << 8)); };
		auto u32 = [&](unsigned i) {
			return uint32_t(u8(i)) | (uint32_t(u8(i + 1)) << 8) | (uint32_t(u8(i + 2)) << 16)
			       | (uint32_t(u8(i + 3)) << 24);
		};
		const bool valid = result == FR_OK && actual == readHeader_.size()
		                   && std::memcmp(readHeader_.data(), "RIFF", 4) == 0
		                   && std::memcmp(readHeader_.data() + 8, "WAVEfmt ", 8) == 0 && u32(16) == 16 && u16(20) == 1
		                   && u16(22) == 2 && u32(24) == 44100 && u32(28) == 264600 && u16(32) == 6 && u16(34) == 24
		                   && std::memcmp(readHeader_.data() + 36, "data", 4) == 0 && u32(40) % 6 == 0
		                   && u32(4) == u32(40) + 36 && f_size(&reader_) == uint64_t(u32(40)) + 44;
		if (!valid) {
			readerStep_ = ReaderStep::Closing;
			return Status::StorageError;
		}
		readTotalFrames_ = u32(40) / 6;
		readerStep_ = ReaderStep::Data;
		totalFrames = readTotalFrames_;
		return Status::Busy;
	}
	if (readerStep_ != ReaderStep::Data)
		return Status::Busy;
	totalFrames = readTotalFrames_;
	if (frames.empty()) {
		if (first == 0 && readNextFrame_ == 0)
			return Status::Ok; // Header is validated; keep the stream open for sequential data.
		if (first != readTotalFrames_ || readNextFrame_ != readTotalFrames_)
			return Status::Invalid;
		readerCall_ = true;
		const auto closeResult = f_close(&reader_);
		readerCall_ = false;
		readerOpen_ = false;
		const bool canceled = readerCancel_;
		readerStep_ = ReaderStep::Idle;
		readerCancel_ = false;
		readSlot_ = kInvalidSlot;
		readIdentity_ = 0;
		return canceled ? Status::Stale : closeResult == FR_OK ? Status::Ok : Status::StorageError;
	}
	if (first != readNextFrame_ || first > totalFrames || frames.size() > totalFrames - first)
		return Status::Invalid;
	const UINT requested = static_cast<UINT>(frames.size() * 6);
	UINT actual = 0;
	readerCall_ = true;
	const auto result = f_read(&reader_, readBytes_.data(), requested, &actual);
	readerCall_ = false;
	if (actual % 6 != 0) {
		readerStep_ = ReaderStep::Closing;
		return Status::StorageError;
	}
	framesRead = actual / 6;
	for (size_t i = 0; i < framesRead; ++i) {
		auto decode = [&](unsigned offsetByte) {
			int32_t value = int32_t(std::to_integer<uint8_t>(readBytes_[i * 6 + offsetByte]))
			                | (int32_t(std::to_integer<uint8_t>(readBytes_[i * 6 + offsetByte + 1])) << 8)
			                | (int32_t(std::to_integer<uint8_t>(readBytes_[i * 6 + offsetByte + 2])) << 16);
			if (value & 0x800000)
				value |= int32_t(0xff000000);
			return value * 256;
		};
		if (!readerCancel_)
			frames[i] = {decode(0), decode(3)};
	}
	if (readerCancel_) {
		framesRead = 0;
		readerStep_ = ReaderStep::Closing;
		return Status::Stale;
	}
	readNextFrame_ += static_cast<FrameIndex>(framesRead);
	if (result != FR_OK || actual != requested) {
		readerStep_ = ReaderStep::Closing;
		return Status::StorageError;
	}
	return Status::Ok;
}

void FatFsArchiveStorage::cancelCheckpointRead(SlotId slot, Generation identity) {
	if (readerStep_ != ReaderStep::Idle && readSlot_ == slot && readIdentity_ == identity) {
		readerCancel_ = true;
		// Do not clear a state while a yielding FatFs call may still publish an
		// open FIL or advance the stream when it returns.
		if (!readerCall_)
			readerStep_ = ReaderStep::Closing;
	}
}

Status FatFsArchiveStorage::serviceCheckpointReadCleanup() {
	if (readerStep_ == ReaderStep::Idle)
		return Status::Ok;
	if (readerCall_)
		return Status::Busy;
	if (readerStep_ != ReaderStep::Closing && !readerCancel_)
		return Status::Busy;
	readerCall_ = readerOpen_;
	const auto result = readerOpen_ ? f_close(&reader_) : FR_OK;
	readerCall_ = false;
	if (readerOpen_) {
		readerOpen_ = false; // Abandon after a terminal close failure; never retry a dead FIL.
		readerCleanupFailed_ |= result != FR_OK;
		return readerCleanupFailed_ ? Status::StorageError : Status::Busy;
	}
	const auto allocationCleanup = f_archive_work_cleanup(&readWork_, 128);
	if (allocationCleanup == FR_ARCHIVE_RETRY)
		return Status::Busy;
	const bool allocationFailed = allocationCleanup != FR_OK;
	readerStep_ = ReaderStep::Idle;
	readerCancel_ = false;
	readSlot_ = kInvalidSlot;
	readIdentity_ = 0;
	readNextFrame_ = readTotalFrames_ = 0;
	const bool failed = readerCleanupFailed_ || allocationFailed;
	readerCleanupFailed_ = false;
	return failed ? Status::StorageError : Status::Ok;
}

bool FatFsArchiveStorage::checkpointReadIdle() const {
	return readerStep_ == ReaderStep::Idle && !readerOpen_;
}
void FatFsArchiveStorage::cleanupError(SlotId slot, FRESULT result) {
	if (!validSlot(slot) || cleanup_[slot].error != MediaError::None)
		return;
	cleanup_[slot] = {result == FR_NOT_READY || result == FR_NOT_ENABLED || result == FR_INVALID_DRIVE
	                      ? MediaError::Absent
	                  : result == FR_WRITE_PROTECTED ? MediaError::Protected
	                                                 : MediaError::Io,
	                  uint32_t(result), 0};
}
void FatFsArchiveStorage::discard(SlotId slot, Generation identity) {
	requestDiscard(slot, identity, true);
}
void FatFsArchiveStorage::requestDiscard(SlotId slot, Generation identity, bool resetDiagnostics) {
	if (!validSlot(slot) || files_[slot].identity != identity || files_[slot].deleting || files_[slot].transferred)
		return;
	files_[slot].deleting = true;
	files_[slot].removal = {};
	if (resetDiagnostics)
		cleanup_[slot] = {};
}
Status FatFsArchiveStorage::serviceDiscard(SlotId slot, Generation identity) {
	if (!validSlot(slot) || files_[slot].identity != identity || files_[slot].transferred)
		return Status::Ok;
	auto& target = files_[slot];
	if (!target.deleting)
		requestDiscard(slot, identity, true);
	const auto result = f_unlink_archive(target.path, &target.removal, 128);
	if (result == FR_ARCHIVE_RETRY)
		return Status::Busy;
	target.identity = 0;
	target.transferred = false;
	target.deleting = false;
	target.removal = {};
	if (result != FR_OK && result != FR_NO_FILE && result != FR_NO_PATH)
		cleanupError(slot, result);
	return cleanup_[slot].error == MediaError::None ? Status::Ok : Status::StorageError;
}
} // namespace loop_station
