#include "diagnostics.h"
#include <algorithm>
namespace loop_station {
void Diagnostics::configure(const MemoryReservation& memory) {
	value_ = {};
	value_.profile = memory.profile();
	value_.activeBytes = memory.activeStorage().size();
	value_.historyBytes = memory.historyStorage().size();
	value_.snapshotBytes = memory.snapshotStorage().size();
	value_.gestureBytes = memory.gestureStorage().size();
	value_.stagingBytes = memory.stagingStorage().size();
	value_.reservedBytes =
	    value_.activeBytes + value_.historyBytes + value_.snapshotBytes + value_.gestureBytes + value_.stagingBytes;
	const std::span<std::byte> spans[] = {memory.activeStorage(), memory.historyStorage(), memory.snapshotStorage(),
	                                      memory.gestureStorage(), memory.stagingStorage()};
	for (unsigned i = 0; i < 5; ++i)
		value_.arenaAddresses[i] = reinterpret_cast<uintptr_t>(spans[i].data());
	value_.status = Status::Ok;
}
void Diagnostics::callback(uint32_t cycles, uint32_t frames) {
	++value_.callbacks;
	value_.callbackCycles += cycles;
	value_.maxCallbackCycles = std::max(value_.maxCallbackCycles, uint64_t(cycles));
	value_.renderedFrames += frames;
}
void Diagnostics::track(SlotId slot, uint32_t frames, bool capture, bool playing) {
	if (!validSlot(slot))
		return;
	value_.capturedFrames[slot] += capture ? frames : 0;
	value_.playbackFrames[slot] += playing ? frames : 0;
	const auto bit = uint8_t(1u << slot);
	value_.captureMask = uint8_t((value_.captureMask & ~bit) | (capture ? bit : 0));
	value_.playingMask = uint8_t((value_.playingMask & ~bit) | (playing ? bit : 0));
}
void Diagnostics::work(uint64_t pages, uint64_t frames, uint64_t pressure, uint32_t historyPages,
                       uint32_t snapshotPages) {
	value_.pageCopyPages = pages;
	value_.pageCopyFrames = frames;
	value_.historyPressure = pressure;
	value_.historyPages = historyPages;
	value_.snapshotPages = snapshotPages;
}
} // namespace loop_station
