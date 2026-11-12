#include "fixture.h"
#if LOOP_STATION_TEST_MODE
#include <algorithm>
#include <array>
namespace loop_station::feasibility {
Status Fixture::start(MemoryProfile profile, FrameIndex phraseFrames) {
	if (session_)
		return Status::Busy;
	if (!phraseFrames || uint64_t(phraseFrames) * sizeof(Frame) > activeAudioBytes(profile) / kTrackCount)
		return Status::Invalid;
	auto status = memory_.acquire(profile);
	if (status != Status::Ok) {
		diagnostics_.configure(memory_);
		diagnostics_.status(status);
		return status;
	}
	diagnostics_.configure(memory_);
	session_.emplace(memory_.pin());
	auto& s = *session_;
	status = s.pool.initialize(memory_.activeStorage());
	if (status == Status::Ok)
		status = s.rollback.initialize(memory_.gestureStorage());
	if (status == Status::Ok)
		status = s.history.initialize(memory_.historyStorage());
	if (status == Status::Ok)
		status = s.snapshots.initialize(memory_.snapshotStorage());
	for (SlotId slot = 0; status == Status::Ok && slot < kTrackCount; ++slot)
		status = s.transpose.prepare(slot);
	if (status != Status::Ok) {
		stop();
		diagnostics_.status(status);
		return status;
	}
	s.audio.setWriteObservers(&s.history, &s.snapshots);
	auto settings = defaultSettings();
	settings.startAction.mode = ActionMode::Immediate;
	settings.stopAction.mode = ActionMode::Immediate;
	s.engine.dispatch(Command::configure(settings), 0, s.clock);
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		s.transpose.setCents(slot, settings.tracks[slot].transposeCents);
	now_ = 0;
	initialFrames_ = phraseFrames;
	nextToggle_ = uint64_t(phraseFrames) + kSampleRate;
	seeded_ = false;
	archiveEnabled_ = false;
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		status = s.engine.dispatch({CommandType::Press, slot}, now_, s.clock);
		if (status != Status::Ok) {
			stop();
			diagnostics_.status(status);
			return status;
		}
	}
	return Status::Ok;
}
void Fixture::stop() {
	// Serialized foreground lifecycle: stop callbacks before destroying consumers.
	session_.reset();
	memory_.release();
	diagnostics_.status(Status::Unavailable);
}
Status Fixture::toggleCapture() {
	if (!session_ || !seeded_)
		return Status::Unavailable;
	auto& s = *session_;
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		const auto status = s.engine.dispatch({CommandType::Press, slot}, now_, s.clock);
		if (status != Status::Ok) {
			diagnostics_.status(status);
			return status;
		}
	}
	return Status::Ok;
}
Status Fixture::setTransposeCents(int32_t totalCents) {
	if (!session_)
		return Status::Unavailable;
	auto& s = *session_;
	auto settings = s.engine.settings();
	for (auto& track : settings.tracks)
		track.transposeCents =
		    static_cast<int16_t>(std::clamp(totalCents, int32_t(Transpose::kMinCents), int32_t(Transpose::kMaxCents)));
	const auto status = s.engine.dispatch(Command::configure(settings), now_, s.clock);
	if (status == Status::Ok)
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			s.transpose.setCents(slot, settings.tracks[slot].transposeCents);
	return status;
}
void Fixture::callback(std::span<const Frame> input) {
	if (!session_ || input.empty())
		return;
	const auto begin = counter_ ? counter_() : 0;
	const auto total = input.size();
	auto& s = *session_;
	std::array<Frame, 128> output;
	while (!input.empty()) {
		if (!seeded_ && now_ == initialFrames_) {
			for (SlotId slot = 0; slot < kTrackCount; ++slot) {
				const auto status = s.engine.dispatch({CommandType::Press, slot}, now_, s.clock);
				if (status != Status::Ok)
					diagnostics_.status(status);
			}
			seeded_ = true;
			s.engine.dispatch({CommandType::StartAll}, now_, s.clock);
		}
		if (seeded_ && now_ == nextToggle_) {
			toggleCapture();
			nextToggle_ += kSampleRate;
		}
		const auto boundary = seeded_ ? nextToggle_ : initialFrames_;
		auto count = std::min<size_t>({input.size(), output.size(), size_t(boundary - now_)});
		s.adapter.finalizeWindow(now_, count, s.clock);
		for (SlotId slot = 0; slot < kTrackCount; ++slot) {
			s.adapter.renderDry(slot, input.first(count), std::span(output).first(count));
			const auto state = s.engine.state(slot);
			diagnostics_.track(slot, count, state.capture, state.playing);
		}
		now_ += count;
		input = input.subspan(count);
	}
	const auto& history = s.history.work();
	const auto& snapshots = s.snapshots.work();
	diagnostics_.work(history.copiedPages + snapshots.copiedPages, history.copiedFrames + snapshots.copiedFrames,
	                  s.history.pressure(), s.history.retainedPages(), s.snapshots.retainedPages());
	diagnostics_.callback(counter_ ? uint32_t(counter_() - begin) : 0, total);
}
void Fixture::serviceArchive() {
	if (!archiveEnabled_ || !sink_ || !session_ || !seeded_)
		return;
	// Keep staging alive even if an injected yielding sink disables the fixture.
	auto pin = memory_.pin();
	if (!pin.valid())
		return;
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		auto& s = *session_;
		SnapshotId id;
		if (s.snapshots.request(slot, s.rollback.boundGeneration(slot), id) != Status::Ok)
			continue;
		const auto count = std::min<size_t>(s.engine.state(slot).phraseFrames, SnapshotStore::kStagingFrames);
		auto staging = std::span(reinterpret_cast<Frame*>(memory_.stagingStorage().data()), count);
		const auto read = s.snapshots.read(id, 0, staging);
		if (read == Status::Ok) {
			diagnostics_.storageCall();
			const auto status = sink_->write(staging);
			if (!session_)
				return;
			// A yielding sink must never publish an invalidated snapshot. This
			// benchmark writes only one bounded block and publishes no archive.
			if (status != Status::Ok || !session_->snapshots.valid(id))
				diagnostics_.status(status == Status::Ok ? Status::Stale : status);
		}
		session_->snapshots.release(id);
	}
}
} // namespace loop_station::feasibility
#endif

#if LOOP_STATION_TEST_MODE && !LOOP_STATION_HOST_TEST
#include "RZA1/cpu_specific.h"
#include "definitions.h"
#include "io/debug/log.h"
#include "io/debug/print.h"
#include "memory/general_memory_allocator.h"
#include <new>
namespace loop_station::feasibility {
namespace {
class FirmwareAllocator final : public ReservationAllocator {
public:
	void* allocatePinnedLowSpeed(size_t bytes) override {
		auto& allocator = GeneralMemoryAllocator::get();
		auto* address = allocator.allocLowSpeed(bytes);
		if (!address)
			return nullptr;
		const auto first = reinterpret_cast<uintptr_t>(address);
		const auto region = allocator.getRegion(address);
		const auto header = *(reinterpret_cast<const uint32_t*>(address) - 1);
		// Validate the actual allocation, not merely a static linker section.
		if (first < EXTERNAL_MEMORY_BEGIN || first >= EXTERNAL_MEMORY_END || bytes > EXTERNAL_MEMORY_END - first
		    || (header & SPACE_TYPE_MASK) != SPACE_HEADER_ALLOCATED
		    || (region != MEMORY_REGION_EXTERNAL && region != MEMORY_REGION_STEALABLE)) {
			allocator.dealloc(address);
			return nullptr;
		}
		D_PRINTLN("loop reserve %08x..%08x bytes=%u region=%d pinned=1", unsigned(first), unsigned(first + bytes),
		          unsigned(bytes), region);
		return address;
	}
	void deallocate(void* address) override { GeneralMemoryAllocator::get().dealloc(address); }
};
// Object metadata is fixed SDRAM, never a multi-megabyte internal .bss array.
alignas(Fixture) PLACE_SDRAM_BSS std::byte firmwareFixtureStorage[sizeof(Fixture)];
FirmwareAllocator firmwareAllocator;
} // namespace
Fixture* firmwareFixture = nullptr;
void firmwareInitialize() {
	if (firmwareFixture)
		return;
	Debug::init();
	firmwareFixture = new (firmwareFixtureStorage) Fixture(firmwareAllocator, [] { return Debug::readCycleCounter(); });
	const auto status = firmwareFixture->start(kConfiguredMemoryProfile);
	D_PRINTLN("loop feasibility profile=%u status=%u gesture=%u object=%u", unsigned(kConfiguredMemoryProfile),
	          unsigned(status), unsigned(GestureRollback::requiredBytes()), unsigned(sizeof(Fixture)));
}
void firmwareCallback(size_t frames) {
	if (!firmwareFixture)
		return;
	// Synthetic dry source: deterministic backing/capture load, no product mixing.
	std::array<Frame, 128> input;
	input.fill({4096, -4096});
	while (frames) {
		const auto count = std::min(frames, input.size());
		firmwareFixture->callback(std::span(input).first(count));
		frames -= count;
	}
}
void firmwareServiceArchive() {
	if (firmwareFixture)
		firmwareFixture->serviceArchive();
}
} // namespace loop_station::feasibility
#endif
