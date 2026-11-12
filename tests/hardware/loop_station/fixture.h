#pragma once
#ifndef LOOP_STATION_TEST_MODE
#define LOOP_STATION_TEST_MODE 0
#endif
#if LOOP_STATION_TEST_MODE
#if !IN_HARDWARE_DEBUG && !LOOP_STATION_HOST_TEST
#error "Loop station feasibility harness requires a debug build"
#endif
#include "deluge/loop_station/audio_adapter.h"
#include "deluge/loop_station/diagnostics.h"
#include "deluge/loop_station/engine.h"
#include "deluge/loop_station/history.h"
#include "deluge/loop_station/snapshot.h"
#include "deluge/loop_station/transpose.h"
#include <optional>
namespace loop_station::feasibility {
class ArchiveSink {
public:
	virtual ~ArchiveSink() = default;
	virtual Status write(std::span<const Frame> frames) = 0;
};
class Fixture {
public:
	using CycleCounter = uint32_t (*)();
	Fixture(ReservationAllocator& allocator, CycleCounter counter, ArchiveSink* sink = nullptr)
	    : memory_(allocator), counter_(counter), sink_(sink) {}
	~Fixture() { stop(); }
	Status start(MemoryProfile profile, FrameIndex phraseFrames = kTenSecondFrames);
	void stop();
	void callback(std::span<const Frame> input);
	Status toggleCapture();
	Status setTransposeCents(int32_t totalCents);
	void setArchiveEnabled(bool enabled) { archiveEnabled_ = enabled; }
	void serviceArchive();
	const Diagnostics& diagnostics() const { return diagnostics_; }

private:
	struct Session {
		explicit Session(MemoryReservation::Pin pin) : pin(std::move(pin)) {}
		MemoryReservation::Pin pin; // Destroyed after all arena consumers.
		PagePool pool;
		GestureRollback rollback{pool};
		History history{pool, rollback};
		SnapshotStore snapshots{pool, rollback};
		ResidentAudio audio{pool, rollback};
		Transpose transpose;
		Engine engine{audio, rollback};
		AudioAdapter adapter{engine, audio, pool, transpose};
		ClockMap clock;
	};
	MemoryReservation memory_;
	std::optional<Session> session_;
	Diagnostics diagnostics_;
	CycleCounter counter_;
	ArchiveSink* sink_;
	SampleClock now_ = 0, nextToggle_ = 0;
	FrameIndex initialFrames_ = 0;
	bool seeded_ = false, archiveEnabled_ = false;
};
extern Fixture* firmwareFixture;
void firmwareInitialize();
void firmwareCallback(size_t frames);
// Debugger/foreground only; default disabled, no product controls or SD writer.
void firmwareServiceArchive();
} // namespace loop_station::feasibility
#endif
