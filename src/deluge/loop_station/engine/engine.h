/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "loop_station/model/types.h"

#include <array>

namespace deluge::loop_station::core {

struct TransportTime {
	int64_t performedTick = 0;
	int64_t barLength = 1;
	int64_t eighthLength = 1;
	bool running = false;
};

struct ScheduledEvent {
	PendingAction action = PendingAction::NONE;
	int64_t tick = 0;
	[[nodiscard]] bool pending() const { return action != PendingAction::NONE; }
};

struct TrackRuntime {
	TrackState state = TrackState::EMPTY;
	TrackState stableState = TrackState::EMPTY;
	CapturePhase capturePhase = CapturePhase::NONE;
	ScheduledEvent pending{};
	int64_t captureStartTick = 0;
	int64_t phraseLengthTicks = 0;
	int64_t playbackOriginTick = 0;
	int64_t savedStopPhaseTicks = 0;
	int64_t overdubStartTick = 0;
	int64_t overdubEndTick = 0;
	int64_t fadeEndTick = 0;
	Error error = Error::NONE;
	uint32_t lastPressMs = UINT32_MAX;
	uint32_t transaction = 0;
	bool dependentOverdub = false;
	bool playingWhileFinalizing = false;
};

struct CaptureResult {
	bool accepted = true;
	bool asynchronous = false;
	int64_t actualPhraseLengthTicks = 0;
};

class EngineDelegate {
public:
	virtual ~EngineDelegate() = default;
	virtual bool preflight(SlotId, bool overdub) = 0;
	virtual bool beginCapture(SlotId, bool overdub, int64_t tick, int64_t length) = 0;
	virtual CaptureResult endCapture(SlotId, bool overdub, int64_t length, bool cropToLength) = 0;
	virtual void abortProvisionalCapture(SlotId, bool overdub) = 0;
	virtual void publishStableState(SlotId, TrackState, int64_t phase) = 0;
	virtual bool navigateRevision(SlotId, bool redo, bool targetHasPhrase) = 0;
	virtual void clearRevisions(SlotId) = 0;
};

class Engine {
public:
	explicit Engine(SongModel& model, EngineDelegate& delegate);

	[[nodiscard]] TrackRuntime const& track(SlotId slot) const { return runtime_[indexOf(slot)]; }
	[[nodiscard]] TrackRuntime& track(SlotId slot) { return runtime_[indexOf(slot)]; }
	void restore(SlotId slot, int64_t phraseLengthTicks, int64_t playbackOriginTick, bool playing);

	// Centre down handles all states except STOPPED. centreUp handles a stopped
	// tap; clearHold is called at the two-second threshold.
	void centreDown(SlotId slot, TransportTime const& time, uint32_t gestureMs);
	void centreUp(SlotId slot, TransportTime const& time, uint32_t heldMs);
	void clearHold(SlotId slot);
	bool undo(SlotId slot);
	bool redo(SlotId slot);
	void process(TransportTime const& time);
	void commitCompleted(SlotId slot, bool success, Error error = Error::NONE);
	void fail(SlotId slot, Error error);

	void requestTransportStop(TransportTime const& time);
	void requestExit(TransportTime const& time);
	[[nodiscard]] bool shouldKeepClockRunning() const;
	[[nodiscard]] bool exitReady() const;
	[[nodiscard]] int64_t ticksUntilNextEvent(int64_t now) const;

	[[nodiscard]] static int64_t nextBar(int64_t tick, int64_t barLength, bool includeBoundary);
	[[nodiscard]] static int64_t smartStop(int64_t tick, int64_t barLength, int64_t eighthLength);

private:
	void primary(SlotId slot, TransportTime const& time);
	void secondary(SlotId slot, TransportTime const& time);
	void startInitial(SlotId slot, TransportTime const& time);
	void beginCapture(SlotId slot, bool overdub, int64_t tick, int64_t length = 0);
	void finishInitial(SlotId slot, TransportTime const& time);
	void finishCapture(SlotId slot, bool overdub, int64_t tick);
	void requestStop(SlotId slot, TransportTime const& time);
	void stopNow(SlotId slot, int64_t tick);
	void resume(SlotId slot, TransportTime const& time);
	void execute(SlotId slot, PendingAction action, TransportTime const& time);
	void setState(SlotId slot, TrackState state, int64_t phase = 0);
	void enterError(SlotId slot, Error error);
	bool navigate(SlotId slot, bool redo);
	[[nodiscard]] bool isBusy(TrackRuntime const& runtime) const;

	SongModel& model_;
	EngineDelegate& delegate_;
	std::array<TrackRuntime, kSlotCount> runtime_{};
	bool exitRequested_ = false;
	bool transportStopRequested_ = false;
	uint32_t nextTransaction_ = 1;
};

} // namespace deluge::loop_station::core
