#include "memory_reservation.h"
#include "history.h"
#include "invariant.h"
namespace loop_station {
MemoryReservation::~MemoryReservation() {
	// Owner must outlive every consumer, including any pending-release pin.
	detail::invariant(pins_ == 0);
	release();
}
MemoryReservation::Pin::~Pin() {
	if (owner_ && --owner_->pins_ == 0 && owner_->pendingRelease_)
		owner_->freeStorage();
}
MemoryReservation::Pin::Pin(Pin&& other) noexcept : owner_(other.owner_) {
	other.owner_ = nullptr;
}
Status MemoryReservation::acquire(MemoryProfile requested) {
	if (!activeStorage().empty() || pins_ || pendingRelease_)
		return Status::Busy;
	const auto active = activeAudioBytes(requested);
	if (!active)
		return Status::Invalid;
	profile_ = requested;
	const size_t sizes[] = {active, History::kCandidateBytes, detail::BeforeImages::kMaxPages * kPageBytes,
	                        GestureRollback::requiredBytes(), kPageBytes};
	for (unsigned i = 0; i < 5; ++i) {
		auto* address = static_cast<std::byte*>(allocator_.allocatePinnedLowSpeed(sizes[i]));
		if (!address) {
			freeStorage();
			return Status::NoMemory;
		}
		storage_[i] = {address, sizes[i]};
	}
	return Status::Ok;
}
void MemoryReservation::release() {
	pendingRelease_ = true;
	if (!pins_)
		freeStorage();
}
MemoryReservation::Pin MemoryReservation::pin() {
	if (activeStorage().empty() || pendingRelease_)
		return {};
	++pins_;
	return Pin(this);
}
void MemoryReservation::freeStorage() {
	for (auto& storage : storage_) {
		if (!storage.empty())
			allocator_.deallocate(storage.data());
		storage = {};
	}
	pendingRelease_ = false;
}
} // namespace loop_station
