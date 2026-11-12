#pragma once
#include "gesture_rollback.h"
namespace loop_station {
// Called only by the serialized foreground owner, never by render or an ISR.
class ReservationAllocator {
public:
	virtual ~ReservationAllocator() = default;
	virtual void* allocatePinnedLowSpeed(size_t bytes) = 0;
	virtual void deallocate(void* address) = 0;
};
// Owns five disjoint arenas. The allocator and reservation must outlive every
// Pin. Keep one pin until ALL phrases/readers/arena consumers are destroyed.
// Calls, pin destruction and teardown are serialized foreground operations.
class MemoryReservation {
public:
	explicit MemoryReservation(ReservationAllocator& allocator) : allocator_(allocator) {}
	~MemoryReservation();
	MemoryReservation(const MemoryReservation&) = delete;
	MemoryReservation& operator=(const MemoryReservation&) = delete;
	class Pin {
	public:
		Pin() = default;
		~Pin();
		Pin(const Pin&) = delete;
		Pin& operator=(const Pin&) = delete;
		Pin(Pin&& other) noexcept;
		bool valid() const { return owner_ != nullptr; }

	private:
		friend class MemoryReservation;
		explicit Pin(MemoryReservation* owner) : owner_(owner) {}
		MemoryReservation* owner_ = nullptr;
	};
	Status acquire(MemoryProfile requested);
	// Deferred while consumers are pinned; rejects new pins until reclamation.
	void release();
	Pin pin();
	MemoryProfile profile() const { return profile_; }
	std::span<std::byte> activeStorage() const { return storage_[0]; }
	std::span<std::byte> historyStorage() const { return storage_[1]; }
	std::span<std::byte> snapshotStorage() const { return storage_[2]; }
	std::span<std::byte> gestureStorage() const { return storage_[3]; }
	std::span<std::byte> stagingStorage() const { return storage_[4]; }

private:
	void freeStorage();
	ReservationAllocator& allocator_;
	std::span<std::byte> storage_[5]{};
	MemoryProfile profile_ = kConfiguredMemoryProfile;
	unsigned pins_ = 0;
	bool pendingRelease_ = false;
};
} // namespace loop_station
