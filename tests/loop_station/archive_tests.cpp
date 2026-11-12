#include "before_image_fixture.h"
#include "deluge/loop_station/archive.h"
#include "deluge/loop_station/archive_storage.h"
#include "mocks/storage.h"
#include <functional>
#include <utility>
namespace {
struct Storage : ArchiveStorage {
	std::vector<std::byte> data;
	std::function<void()> yielding;
	unsigned begins = 0, finals = 0, aborts = 0, discards = 0;
	bool shortWrite = false;
	Status failure = Status::Ok;
	Status begin(SlotId, Generation) override {
		++begins;
		data.clear();
		return failure;
	}
	Status write(std::span<const std::byte> bytes, size_t& written) override {
		written = bytes.size() - (shortWrite ? 1 : 0);
		data.insert(data.end(), bytes.begin(), bytes.begin() + written);
		if (yielding) {
			auto fn = std::exchange(yielding, {});
			fn();
		}
		return failure;
	}
	Status finalize() override {
		++finals;
		if (yielding) {
			auto fn = std::exchange(yielding, {});
			fn();
		}
		return failure;
	}
	void abort() override { ++aborts; }
	void discard(SlotId, Generation) override { ++discards; }
};
} // namespace
TEST_GROUP(LoopArchive), BeforeImageFixture {
	Storage storage;
	Archive archive{pool, rollback, snapshots};
	void setup() override {
		archive_mock::storage = {};
		BeforeImageFixture::setup();
		CHECK(Status::Ok == snapshots.initialize(arena));
		archive.setStorage(&storage);
	}
	void complete() {
		for (unsigned i = 0; i < 100; ++i)
			archive.service(4096);
	}
};
TEST(LoopArchive, serializesHeaderAndSigned24BitFramesIntoReadyCandidate) {
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	CHECK(ArchiveState::ReadyForPublication == archive.state(0));
	CHECK_EQUAL(62, storage.data.size());
	MEMCMP_EQUAL("RIFF", storage.data.data(), 4);
	MEMCMP_EQUAL("WAVEfmt ", storage.data.data() + 8, 8);
	ArchiveCandidate c;
	CHECK(archive.take(0, c));
	CHECK_EQUAL(3, c.frames);
}
TEST(LoopArchive, stableRequestFreezesThePhraseBeforeLaterOverdubWrites) {
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	dub();
	complete();
	CHECK(ArchiveState::ReadyForPublication == archive.state(0));
	const unsigned char firstSample[] = {0, 0, 0, 255, 255, 255};
	MEMCMP_EQUAL(firstSample, storage.data.data() + 44, sizeof(firstSample));
	CHECK_EQUAL(4, pool.frame(h, 0)->l); // Live overdub remains later than the snapshot.
}
TEST(LoopArchive, cancellationDuringYieldNeverFinishesOrChangesLiveAudio) {
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	storage.yielding = [&] {
		archive.cancel(0);
		archive.service(4096);
		dub();
	};
	complete();
	CHECK_EQUAL(0, storage.finals);
	CHECK_EQUAL(1, storage.aborts);
	CHECK_EQUAL(4, pool.frame(h, 0)->l);
}
TEST(LoopArchive, shortSuccessfulWriteFailsAndRetainsRam) {
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	storage.shortWrite = true;
	complete();
	CHECK(ArchiveState::Failed == archive.state(0));
	CHECK_EQUAL(1, pool.frame(h, 0)->l);
}
TEST(LoopArchive, provisionalGestureDefersSnapshotAndBoundedPressureCoalesces) {
	auto h = phrase();
	CHECK(Status::Ok == rollback.begin(0, 0, 0, 3));
	for (unsigned i = 0; i < 100000; ++i)
		CHECK(Status::Ok == archive.request(0, h.generation));
	archive.service(4096);
	CHECK_EQUAL(0, storage.begins);
	CHECK_EQUAL(1, archive.queued());
	CHECK_EQUAL(0, snapshots.activeSnapshots());
	CHECK(Status::Ok == rollback.confirm(0));
	complete();
	CHECK(ArchiveState::ReadyForPublication == archive.state(0));
}
TEST(LoopArchive, actualAdapterPreservesEveryReferencedFileAcrossStorageFaults) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	for (const auto* operation : {"open", "write", "seek", "sync", "close"}) {
		archive_mock::storage = {};
		archive_mock::storage.files["SAMPLES/LOOPS/prior.WAV"] = {std::byte{77}};
		archive_mock::storage.failOperation = operation;
		CHECK(Status::Ok == archive.request(0, h.generation));
		complete();
		CHECK(ArchiveState::Failed == archive.state(0));
		CHECK(Status::StorageError == archive.result(0));
		CHECK(fat.firstError() == FR_DISK_ERR);
		CHECK_EQUAL(1, archive_mock::storage.files.size());
		CHECK_EQUAL(77, int(archive_mock::storage.files.begin()->second[0]));
		CHECK_EQUAL(1, pool.frame(h, 0)->l);
	}
}
TEST(LoopArchive, headerRewriteShortSeekAndFullMediaAreExplicit) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	archive_mock::storage.shortSeek = true;
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	CHECK(ArchiveState::Failed == archive.state(0));
	CHECK(fat.firstError() == FR_INT_ERR);
	archive_mock::storage = {};
	archive_mock::storage.shortWrite = true;
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	CHECK(fat.error() == MediaError::Full);
	CHECK_EQUAL(43, fat.bytesWritten());
	archive_mock::storage = {};
	archive_mock::storage.failOperation = "write";
	archive_mock::storage.failAfter = 2;
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	CHECK(ArchiveState::Failed == archive.state(0));
	CHECK_EQUAL(62, fat.bytesWritten());
}
TEST(LoopArchive, absentAndProtectedMediaRemainDifferentWithoutMutatingAudio) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	for (auto error : {FR_NOT_READY, FR_WRITE_PROTECTED}) {
		archive_mock::storage = {};
		archive_mock::storage.failOperation = "open";
		archive_mock::storage.failure = error;
		CHECK(Status::Ok == archive.request(0, h.generation));
		complete();
		CHECK(fat.error() == (error == FR_NOT_READY ? MediaError::Absent : MediaError::Protected));
		CHECK_EQUAL(1, pool.frame(h, 0)->l);
	}
}
TEST(LoopArchive, cancellationAtFinalCloseDeletesOnlyExclusiveCandidate) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	archive_mock::storage.files["old.WAV"] = {std::byte{99}};
	archive_mock::storage.yielding = [&](const char* op) {
		if (std::string(op) == "close")
			archive.cancel(0);
	};
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	CHECK(ArchiveState::Cancelled == archive.state(0));
	CHECK_EQUAL(1, archive_mock::storage.files.size());
	CHECK(archive_mock::storage.files.contains("old.WAV"));
}
TEST(LoopArchive, takenCandidateSurvivesLaterEditsFailureCancelAndNameCollision) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	ArchiveCandidate durable;
	CHECK(archive.take(0, durable));
	CHECK(durable.path[0]);
	fat.abort(); // Ownership was transferred; even a later abort cannot delete it.
	const std::string path = durable.path.data();
	const auto original = archive_mock::storage.files[path];
	dub();
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	ArchiveCandidate next;
	CHECK(archive.take(0, next));
	CHECK(durable.identity != next.identity);
	CHECK(path != next.path.data());
	archive.cancel(0);
	complete();
	CHECK(archive_mock::storage.files[path] == original);
	// Simulate a fresh boot with identity sequence reset and pre-existing candidates.
	Archive reboot{pool, rollback, snapshots};
	reboot.setStorage(&fat);
	CHECK(Status::Ok == reboot.request(0, h.generation));
	for (unsigned i = 0; i < 20; ++i)
		reboot.service(4096);
	ArchiveCandidate collision;
	CHECK(reboot.take(0, collision));
	CHECK(path != collision.path.data());
	CHECK(archive_mock::storage.files[path] == original);
}

TEST(LoopArchive, transferredCheckpointReadsBoundedFramesAndSignExtends24BitPcm) {
	FatFsArchiveStorage fat;
	CHECK(Status::Ok == archive.setStorage(&fat));
	auto h = phrase();
	pool.frame(h, 0)->l = INT32_MIN;
	pool.frame(h, 0)->r = INT32_MAX;
	pool.frame(h, 1)->l = -256;
	pool.frame(h, 1)->r = 256;
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	ArchiveCandidate candidate{};
	CHECK(archive.take(0, candidate));
	Frame decoded[2]{};
	size_t framesRead = 0;
	FrameIndex total = 0;
	Status read = Status::Busy;
	for (unsigned i = 0; i < 20 && read == Status::Busy; ++i)
		read = fat.readCheckpoint(0, candidate.identity, 0, decoded, framesRead, total);
	CHECK(Status::Ok == read);
	fat.cancelCheckpointRead(0, candidate.identity);
	while (!fat.checkpointReadIdle())
		fat.serviceCheckpointReadCleanup();
	CHECK_EQUAL(2, framesRead);
	CHECK_EQUAL(3, total);
	CHECK_EQUAL(INT32_MIN, decoded[0].l);
	CHECK_EQUAL(INT32_MAX & ~255, decoded[0].r);
	CHECK_EQUAL(-256, decoded[1].l);
	CHECK_EQUAL(256, decoded[1].r);
}

TEST(LoopArchive, malformedOrShortCheckpointIsRejectedBeforePublishingAnyFrames) {
	FatFsArchiveStorage fat;
	CHECK(Status::Ok == archive.setStorage(&fat));
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	ArchiveCandidate candidate{};
	CHECK(archive.take(0, candidate));
	auto& file = archive_mock::storage.files[candidate.path.data()];
	Frame decoded[3]{};
	size_t framesRead = 99;
	FrameIndex total = 99;
	file[8] = std::byte{'X'};
	Status read = Status::Busy;
	for (unsigned i = 0; i < 20 && read == Status::Busy; ++i)
		read = fat.readCheckpoint(0, candidate.identity, 0, decoded, framesRead, total);
	CHECK(Status::StorageError == read);
	while (!fat.checkpointReadIdle())
		fat.serviceCheckpointReadCleanup();
	CHECK_EQUAL(0, framesRead);
	CHECK_EQUAL(0, total);
	file[8] = std::byte{'W'};
	file.resize(44 + 2 * 6); // Header still declares three frames.
	read = Status::Busy;
	for (unsigned i = 0; i < 20 && read == Status::Busy; ++i)
		read = fat.readCheckpoint(0, candidate.identity, 0, decoded, framesRead, total);
	CHECK(Status::StorageError == read);
	while (!fat.checkpointReadIdle())
		fat.serviceCheckpointReadCleanup();
	CHECK_EQUAL(0, framesRead);
}

TEST(LoopArchive, fullPublishedPathTableKeepsLastTransferredIdentityReadableAndRejectsMore) {
	FatFsArchiveStorage fat;
	CHECK(Status::Ok == archive.setStorage(&fat));
	auto h = phrase();
	ArchiveCandidate last{};
	for (unsigned i = 0; i < 65; ++i) {
		CHECK(Status::Ok == archive.request(0, h.generation));
		complete();
		ArchiveCandidate candidate{};
		CHECK(archive.take(0, candidate));
		last = candidate;
	}
	Frame frame{};
	size_t framesRead = 0;
	FrameIndex totalFrames = 0;
	Status read = Status::Busy;
	for (unsigned i = 0; i < 20 && read == Status::Busy; ++i)
		read = fat.readCheckpoint(0, last.identity, 0, {&frame, 1}, framesRead, totalFrames);
	CHECK(Status::Ok == read);
	fat.cancelCheckpointRead(0, last.identity);
	while (!fat.checkpointReadIdle())
		fat.serviceCheckpointReadCleanup();
	CHECK_EQUAL(1, framesRead);
	CHECK_EQUAL(3, totalFrames);
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	CHECK(ArchiveState::Failed == archive.state(0));
}

TEST(LoopArchive, partialCheckpointReadReportsPrefixAndFailsTheWholeCandidate) {
	FatFsArchiveStorage fat;
	CHECK(Status::Ok == archive.setStorage(&fat));
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	ArchiveCandidate candidate{};
	CHECK(archive.take(0, candidate));
	archive_mock::storage.shortRead = true;
	Frame frames[3]{};
	size_t framesRead = 0;
	FrameIndex totalFrames = 0;
	Status read = Status::Busy;
	for (unsigned i = 0; i < 20 && read == Status::Busy; ++i)
		read = fat.readCheckpoint(0, candidate.identity, 0, frames, framesRead, totalFrames);
	CHECK(Status::StorageError == read);
	while (!fat.checkpointReadIdle())
		fat.serviceCheckpointReadCleanup();
	CHECK_EQUAL(2, framesRead);
	CHECK_EQUAL(3, totalFrames);
}
TEST(LoopArchive, liveOverdubWhileWritingRetainsCoherentSnapshotAndExactPcmEncoding) {
	auto h = phrase(0, 513);
	pool.frame(h, 0)->l = INT32_MIN;
	pool.frame(h, 0)->r = INT32_MAX;
	CHECK(Status::Ok == archive.request(0, h.generation));
	archive.service(44);
	storage.yielding = [&] { dub(0, 513); };
	complete();
	CHECK(ArchiveState::ReadyForPublication == archive.state(0));
	CHECK_EQUAL(44 + 513 * 6, storage.data.size());
	const unsigned char expected[] = {0, 0, 128, 255, 255, 127};
	MEMCMP_EQUAL(expected, storage.data.data() + 44, sizeof(expected));
	CHECK_EQUAL(2, int(storage.data[44 + 512 * 6]));
}
TEST(LoopArchive, supersedingYieldGetsANewSnapshotBeforeSamePhraseGenerationPublishes) {
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	storage.yielding = [&] {
		dub();
		archive.request(0, h.generation);
	};
	complete();
	CHECK_EQUAL(1, storage.aborts);
	CHECK_EQUAL(1, storage.finals);
	CHECK(ArchiveState::ReadyForPublication == archive.state(0));
	CHECK_EQUAL(2, storage.begins);
}
TEST(LoopArchive, roundRobinAndSmallWorkBudgetDoNotStarveOtherSlots) {
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		auto h = phrase(slot);
		CHECK(Status::Ok == archive.request(slot, h.generation));
	}
	for (unsigned i = 0; i < 100; ++i)
		archive.service(44);
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		CHECK(ArchiveState::ReadyForPublication == archive.state(slot));
		ArchiveCandidate c;
		CHECK(archive.take(slot, c));
	}
	CHECK_EQUAL(8, storage.finals);
	CHECK(archive.idle());
}
TEST(LoopArchive, closePermanentlyRejectsNewRequestsAndDrainsActiveCandidate) {
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	archive.service(44);
	archive.close();
	CHECK(Status::Unavailable == archive.request(0, h.generation));
	complete();
	CHECK_EQUAL(1, storage.aborts);
	CHECK(archive.idle());
	CHECK_EQUAL(0, snapshots.activeSnapshots());
}
TEST(LoopArchive, deletingPhraseDuringYieldRejectsStaleGenerationWithoutAffectingOtherSlot) {
	auto first = phrase();
	auto other = phrase(1);
	CHECK(Status::Ok == archive.request(0, first.generation));
	storage.yielding = [&] {
		audio.stop(0);
		audio.clear(0);
	};
	complete();
	CHECK(ArchiveState::Failed == archive.state(0));
	CHECK(archive.result(0) == Status::Stale);
	CHECK_EQUAL(0, storage.finals);
	CHECK_EQUAL(1, storage.aborts);
	CHECK_EQUAL(1, pool.frame(other, 0)->l);
}
TEST(LoopArchive, supersededReadyCandidateIsDiscardedAndPendingPressureReleasesSnapshot) {
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	for (unsigned i = 0; i < 100000; ++i)
		CHECK(Status::Ok == archive.request(0, h.generation));
	CHECK_EQUAL(1, archive.queued());
	CHECK_EQUAL(1, snapshots.activeSnapshots()); // Coalesced request holds only its latest snapshot.
	complete();
	CHECK_EQUAL(1, storage.discards);
	CHECK_EQUAL(2, storage.finals);
}
TEST(LoopArchive, canceledProvisionalGestureAdmitsOnlyEffectiveOriginals) {
	auto h = phrase();
	pool.frame(h, 0)->l = 256;
	CHECK(Status::Ok == rollback.begin(0, 0, 0, 3));
	dub();
	CHECK(Status::Ok == archive.request(0, h.generation));
	archive.service(4096);
	CHECK_EQUAL(0, storage.begins);
	CHECK(Status::Ok == rollback.cancel(0));
	complete();
	CHECK(ArchiveState::ReadyForPublication == archive.state(0));
	CHECK_EQUAL(1, int(storage.data[44]));
}
TEST(LoopArchive, snapshotArenaPressureRejectsMixedRevisionWhileLiveOverdubContinues) {
	auto h = phrase(0, 2561);
	CHECK(Status::Ok == archive.request(0, h.generation));
	storage.yielding = [&] { dub(0, 2561); };
	complete();
	CHECK(ArchiveState::Failed == archive.state(0));
	CHECK(archive.result(0) == Status::Stale);
	CHECK_EQUAL(0, storage.finals);
	CHECK_EQUAL(4, pool.frame(h, 0)->l);
	CHECK_EQUAL(2564, pool.frame(h, 2560)->l);
}
TEST(LoopArchive, nextSlotStorageSuccessDoesNotErasePriorFailureDiagnostics) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto first = phrase();
	auto second = phrase(1);
	archive_mock::storage.shortWrite = true;
	CHECK(Status::Ok == archive.request(0, first.generation));
	complete();
	archive_mock::storage.shortWrite = false;
	CHECK(Status::Ok == archive.request(1, second.generation));
	complete();
	CHECK(archive.diagnostics(0).error == MediaError::Full);
	CHECK_EQUAL(43, archive.diagnostics(0).bytesWritten);
	CHECK(archive.diagnostics(1).error == MediaError::None);
	CHECK_EQUAL(106, archive.diagnostics(1).bytesWritten);
}
TEST(LoopArchive, readyCandidateCannotBeTakenAfterItsPhraseWasDeletedOrReplaced) {
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	audio.stop(0);
	CHECK(Status::Ok == audio.clear(0));
	phrase();
	ArchiveCandidate candidate;
	CHECK_FALSE(archive.take(0, candidate));
	complete();
	CHECK_EQUAL(1, storage.discards);
}
TEST(LoopArchive, unmountedVolumeIsReportedAsAbsentMedia) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	archive_mock::storage.failOperation = "open";
	archive_mock::storage.failure = FR_NOT_ENABLED;
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	CHECK(archive.diagnostics(0).error == MediaError::Absent);
	CHECK_EQUAL(FR_NOT_ENABLED, archive.diagnostics(0).nativeError);
}
TEST(LoopArchive, deniedCreationUsesOnlyValidMountedFreeClusterCache) {
	FatFsArchiveStorage fat{&archive_mock::storage.filesystem};
	archive.setStorage(&fat);
	auto h = phrase();
	for (const auto* operation : {"open"}) {
		for (unsigned scenario = 0; scenario < 5; ++scenario) {
			archive_mock::storage = {};
			archive_mock::storage.failOperation = operation;
			archive_mock::storage.failure = FR_DENIED;
			auto& fs = archive_mock::storage.filesystem;
			fs.fs_type = scenario == 3 ? 0 : FS_FAT32;
			fs.n_fatent = scenario == 4 ? 1 : 1000;
			fs.free_clst = scenario == 1 ? 1 : scenario == 2 ? UINT32_MAX : 0;
			CHECK(Status::Ok == archive.request(0, h.generation));
			complete();
			CHECK(archive.diagnostics(0).error == (scenario == 0 ? MediaError::Full : MediaError::Io));
			CHECK_EQUAL(FR_DENIED, archive.diagnostics(0).nativeError);
			CHECK_EQUAL(0, archive_mock::storage.calls["getfree"]);
		}
	}
}
TEST(LoopArchive, failedCleanupOfReadySlotCannotContaminateAnotherActiveWriter) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto a = phrase();
	auto b = phrase(1, 513);
	CHECK(Status::Ok == archive.request(0, a.generation));
	complete();
	CHECK(Status::Ok == archive.request(1, b.generation));
	for (unsigned i = 0; i < 5 && archive_mock::storage.open.empty(); ++i)
		archive.service(44);
	CHECK_EQUAL(1, archive_mock::storage.open.size());
	CHECK(ArchiveState::Writing == archive.state(1));
	archive.cancel(0);
	archive_mock::storage.failOperation = "unlink";
	archive_mock::storage.failure = FR_DISK_ERR;
	archive.service(4096);
	complete();
	CHECK(ArchiveState::ReadyForPublication == archive.state(1));
	CHECK(archive.diagnostics(1).error == MediaError::None);
	CHECK_EQUAL(FR_OK, archive.diagnostics(1).nativeError);
	CHECK(archive.cleanupDiagnostics(0).error == MediaError::Io);
	CHECK_EQUAL(FR_DISK_ERR, archive.cleanupDiagnostics(0).nativeError);
	CHECK(archive.cleanupDiagnostics(1).error == MediaError::None);
}
TEST(LoopArchive, cancellationDuringFirstFilenameCollisionStopsFurtherOpenAttempts) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	const std::string old = "L1000100.WAV";
	archive_mock::storage.files[old] = {std::byte{91}};
	archive_mock::storage.yielding = [&](const char* op) {
		if (std::string(op) == "open")
			archive.cancel(0);
	};
	CHECK(Status::Ok == archive.request(0, h.generation));
	for (unsigned i = 0; i < 5 && !archive_mock::storage.calls["open"]; ++i)
		archive.service(4096);
	CHECK_EQUAL(1, archive_mock::storage.calls["open"]);
	complete();
	CHECK_EQUAL(1, archive_mock::storage.calls["open"]);
	CHECK_EQUAL(1, archive_mock::storage.files.size());
	CHECK_EQUAL(91, int(archive_mock::storage.files[old][0]));
	CHECK(ArchiveState::Cancelled == archive.state(0));
}
TEST(LoopArchive, collisionExhaustionIsBoundedToOneOpenAttemptPerService) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	constexpr char hex[] = "0123456789ABCDEF";
	std::string prefix = "L1";
	for (int digit = 3; digit >= 0; --digit)
		prefix += hex[(h.generation >> (digit * 4)) & 15];
	for (unsigned i = 0; i < 256; ++i) {
		std::string path = prefix;
		path += hex[i >> 4];
		path += hex[i & 15];
		path += ".WAV";
		archive_mock::storage.files[path] = {std::byte{37}};
	}
	CHECK(Status::Ok == archive.request(0, h.generation));
	for (unsigned i = 0; i < 260; ++i) {
		const auto before = archive_mock::storage.calls["open"];
		archive.service(4096);
		CHECK(archive_mock::storage.calls["open"] - before <= 1);
	}
	CHECK_EQUAL(256, archive_mock::storage.calls["open"]);
	CHECK_EQUAL(256, archive_mock::storage.files.size());
	CHECK(ArchiveState::Failed == archive.state(0));
	CHECK_EQUAL(FR_EXIST, archive.diagnostics(0).nativeError);
}
TEST(LoopArchive, deniedCreationReturnsBeforeSpaceQueryAndCancellationSkipsTheQuery) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	archive_mock::storage.failOperation = "open";
	archive_mock::storage.failure = FR_DENIED;
	archive_mock::storage.freeClusters = 0;
	CHECK(Status::Ok == archive.request(0, h.generation));
	archive.service(4096);
	CHECK_EQUAL(0, archive_mock::storage.calls["getfree"]);
	archive.cancel(0);
	complete();
	CHECK_EQUAL(0, archive_mock::storage.calls["getfree"]);
	archive_mock::storage = {};
	archive_mock::storage.failOperation = "open";
	archive_mock::storage.failure = FR_DENIED;
	archive_mock::storage.yielding = [&](const char* op) {
		if (std::string(op) == "open")
			archive.cancel(0);
	};
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	CHECK_EQUAL(0, archive_mock::storage.calls["getfree"]);
}
TEST(LoopArchive, cancellationAfterSeekRewriteOrSyncStopsSubsequentFinalizeOperations) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	for (const std::string phase : {"seek", "rewrite", "sync"}) {
		archive_mock::storage = {};
		bool canceled = false;
		archive_mock::storage.yielding = [&](const char* op) {
			const bool match =
			    phase == op
			    || (phase == "rewrite" && std::string(op) == "write" && archive_mock::storage.calls["write"] == 3);
			if (!canceled && match) {
				canceled = true;
				archive.cancel(0);
			}
		};
		CHECK(Status::Ok == archive.request(0, h.generation));
		complete();
		CHECK(canceled);
		CHECK(ArchiveState::Cancelled == archive.state(0));
		CHECK_EQUAL(phase == "seek" ? 2 : 3, archive_mock::storage.calls["write"]);
		CHECK_EQUAL(phase == "sync" ? 1 : 0, archive_mock::storage.calls["sync"]);
		CHECK_EQUAL(1, archive_mock::storage.calls["close"]);
		CHECK_EQUAL(1, archive_mock::storage.calls["unlink"]);
		CHECK_EQUAL(0, archive_mock::storage.files.size());
		CHECK_EQUAL(1, pool.frame(h, 0)->l);
	}
}
TEST(LoopArchive, successfulArchiveHasOneStorageOperationPerServiceAndOrderedFinalize) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	for (unsigned i = 0; i < 30; ++i) {
		const auto before = archive_mock::storage.operations.size();
		archive.service(4096);
		CHECK(archive_mock::storage.operations.size() - before <= 1);
	}
	CHECK(ArchiveState::ReadyForPublication == archive.state(0));
	const char* expected[] = {"open", "write", "write", "seek", "write", "sync", "close"};
	CHECK_EQUAL(std::size(expected), archive_mock::storage.operations.size());
	for (unsigned i = 0; i < std::size(expected); ++i)
		STRCMP_EQUAL(expected[i], archive_mock::storage.operations[i].c_str());
}
TEST(LoopArchive, activeAbortClosesAndUnlinksInSeparateServiceSteps) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	for (unsigned i = 0; i < 5 && archive_mock::storage.open.empty(); ++i)
		archive.service(44);
	CHECK_EQUAL(1, archive_mock::storage.open.size());
	archive.cancel(0);
	for (unsigned i = 0; i < 10; ++i) {
		const auto before = archive_mock::storage.operations.size();
		archive.service(4096);
		CHECK(archive_mock::storage.operations.size() - before <= 1);
	}
	CHECK(archive.idle());
	CHECK_EQUAL(1, archive_mock::storage.calls["close"]);
	CHECK_EQUAL(1, archive_mock::storage.calls["unlink"]);
	CHECK_EQUAL(0, archive_mock::storage.files.size());
}
TEST(LoopArchive, partialAllocationRetriesPreserveEveryHeaderAndPayloadByte) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	pool.frame(h, 0)->l = INT32_MIN;
	pool.frame(h, 0)->r = INT32_MAX;
	archive_mock::storage.retryWrites = {{1, 13}, {2, 7}, {4, 11}, {6, 5}};
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	ArchiveCandidate c;
	CHECK(archive.take(0, c));
	const auto& bytes = archive_mock::storage.files[c.path.data()];
	CHECK_EQUAL(62, bytes.size());
	MEMCMP_EQUAL("RIFF", bytes.data(), 4);
	MEMCMP_EQUAL("data", bytes.data() + 36, 4);
	const unsigned char expected[] = {0, 0, 128, 255, 255, 127};
	MEMCMP_EQUAL(expected, bytes.data() + 44, 6);
	CHECK(fat.error() == MediaError::None);
	CHECK_EQUAL(106, fat.bytesWritten());
}
TEST(LoopArchive, cancellationAfterPartialAllocationRetryClearsTheScanCursor) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	archive_mock::storage.retryWrites[1] = 13;
	CHECK(Status::Ok == archive.request(0, h.generation));
	archive.service(4096);
	archive.service(4096);
	CHECK(fat.allocationSearchPending());
	archive.cancel(0);
	complete();
	CHECK_FALSE(fat.allocationSearchPending());
	CHECK(archive.idle());
	CHECK_EQUAL(0, archive_mock::storage.files.size());
	CHECK_EQUAL(0, archive_mock::storage.calls["sync"]);
}
TEST(LoopArchive, alreadyStartedReadyDeletionRetainsItsCleanupLeaseWhenClosing) {
	FatFsArchiveStorage fat;
	archive.setStorage(&fat);
	auto h = phrase();
	CHECK(Status::Ok == archive.request(0, h.generation));
	complete();
	archive.cancel(0);
	archive_mock::storage.failOperation = "unlink";
	archive_mock::storage.failure = FR_ARCHIVE_RETRY;
	archive.service(4096);
	archive.close();
	CHECK_FALSE(archive.idle());
	complete();
	CHECK(archive.idle());
	CHECK_EQUAL(0, archive_mock::storage.files.size());
}
