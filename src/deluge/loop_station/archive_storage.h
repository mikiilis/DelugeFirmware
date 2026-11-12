#pragma once
#include "archive.h"
#include "fatfs/ff.h"
namespace loop_station {
// One open FatFs stream, foreground only. CREATE_NEW plus bounded collision
// retries ensure a candidate can never overwrite an older song revision.
class FatFsArchiveStorage final : public ArchiveStorage {
public:
	explicit FatFsArchiveStorage(const FATFS* filesystem = nullptr) : filesystem_(filesystem) {}
	Status begin(SlotId, Generation) override;
	Status write(std::span<const std::byte>, size_t&) override;
	Status finalize() override;
	void abort() override;
	Status serviceAbort() override;
	void discard(SlotId, Generation) override;
	Status serviceDiscard(SlotId, Generation) override;
	bool allocationSearchPending() const { return work_.valid; }
	void transfer(SlotId, Generation) override;
	Status readCheckpoint(SlotId, Generation, FrameIndex, std::span<Frame>, size_t&, FrameIndex&) override;
	void cancelCheckpointRead(SlotId, Generation) override;
	Status serviceCheckpointReadCleanup() override;
	bool checkpointReadIdle() const override;
	MediaError error() const override { return error_; }
	StorageDiagnostics diagnostics() const override { return {error_, uint32_t(firstError_), bytesWritten_}; }
	StorageDiagnostics cleanupDiagnostics(SlotId slot) const override {
		return validSlot(slot) ? cleanup_[slot] : StorageDiagnostics{};
	}
	FRESULT firstError() const { return firstError_; }
	uint64_t bytesWritten() const { return bytesWritten_; }
	const char* candidatePath(SlotId slot, Generation identity) const override;
	Status registerPublishedFile(SlotId slot, Generation identity, const char* path);
	void clearPublishedFiles() {
		for (auto& file : published_)
			file = {};
	}

private:
	struct File {
		Generation identity = 0;
		char path[80]{};
		FFArchiveDelete removal{};
		bool deleting = false, transferred = false;
	} files_[kTrackCount]{};
	struct PublishedFile {
		SlotId slot = kInvalidSlot;
		Generation identity = 0;
		char path[80]{};
	} published_[64]{};
	const PublishedFile* publishedFile(SlotId, Generation) const;
	const FATFS* filesystem_;
	Status fail(FRESULT result, bool full = false);
	Status failCreation(FRESULT result);
	void requestDiscard(SlotId, Generation, bool resetDiagnostics);
	void cleanupError(SlotId, FRESULT);
	std::array<StorageDiagnostics, kTrackCount> cleanup_{};
	// One SongModel restore builder uses this foreground-only scratch. It is
	// separate from audio memory and never reachable from render callbacks.
	std::array<std::byte, 512 * 6> readBytes_{};
	FFArchiveWork work_{};
	FIL reader_{};
	FFArchiveWork readWork_{};
	std::array<char, 80> readPath_{};
	FIL file_{};
	std::array<std::byte, 44> header_{};
	std::array<std::byte, 44> readHeader_{};
	SlotId slot_ = kInvalidSlot;
	Generation identity_ = 0;
	MediaError error_ = MediaError::None;
	FRESULT firstError_ = FR_OK;
	uint64_t bytesWritten_ = 0;
	uint16_t collision_ = 0;
	uint8_t finalizeStep_ = 0;
	uint32_t rewriteWritten_ = 0;
	bool beginning_ = false, open_ = false, owns_ = false, haveHeader_ = false;
	SlotId readSlot_ = kInvalidSlot;
	Generation readIdentity_ = 0;
	FrameIndex readNextFrame_ = 0, readTotalFrames_ = 0;
	enum class ReaderStep : uint8_t { Idle, Open, Header, Data, Closing };
	ReaderStep readerStep_ = ReaderStep::Idle;
	bool readerOpen_ = false, readerCancel_ = false, readerCall_ = false, readerCleanupFailed_ = false;
};
} // namespace loop_station
