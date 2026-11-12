#include "engine.h"
namespace loop_station {
void Engine::advanceStreamGeneration(SlotId slot) {
	if (!validSlot(slot))
		return;
	if (++streamGeneration_[slot] == 0)
		++streamGeneration_[slot];
}

void Engine::phraseReplaced(SlotId slot, FrameIndex length, bool playing) {
	if (!validSlot(slot) || !length)
		return;
	advanceStreamGeneration(slot);
	auto& state = slots_[slot].state;
	state.hasPhrase = true;
	state.phraseFrames = length;
	state.capture = false;
	state.playing = playing;
	state.pending = false;
	state.action = PendingAction::None;
	state.error = Status::Ok;
}

void Engine::queue(SlotId slot, PendingAction action, Deadline deadline) {
	auto& s = slots_[slot];
	if (action == PendingAction::Start)
		s.held = false;
	if (action == PendingAction::Stop)
		audio_.setOneShotCompletion(slot, false);
	s.state.action = action;
	s.state.pending = true;
	s.deadline = deadline;
}
void Engine::cancelPending(SlotId slot) {
	if (slots_[slot].state.action == PendingAction::Stop)
		audio_.setOneShotCompletion(slot, settings_.tracks[slot].playback == PlaybackMode::OneShot);
	slots_[slot].state.pending = false;
	slots_[slot].state.action = PendingAction::None;
}
void Engine::close(SlotId slot, bool cancel) {
	if (slots_[slot].state.playing && slots_[slot].state.capture)
		audio_.setCaptureGate(slot, false);
	if (cancel)
		rollback_.cancel(slot);
	else
		rollback_.confirm(slot);
	if (slots_[slot].state.playing)
		slots_[slot].state.capture = false;
}
Status Engine::fail(SlotId slot, Status status) {
	auto& s = slots_[slot];
	close(slot, false);
	if (s.state.capture && !s.state.playing)
		complete(slot, true, false);
	cancelPending(slot);
	s.durationActive = false;
	s.held = false;
	s.tap = false;
	s.state.error = status;
	return status;
}
Status Engine::complete(SlotId slot, bool play, bool dub) {
	auto& s = slots_[slot];
	PhraseHandle handle;
	const auto status = audio_.finishInitial(slot, handle);
	const auto completion = audio_.takeCompletion(slot);
	s.state.capture = false;
	s.durationActive = false;
	if (status != Status::Ok) {
		// Zero-frame takes remain empty, without manufacturing a capture fault.
		s.state.hasPhrase = false;
		s.state.playing = false;
		s.state.phraseFrames = 0;
		return completion.frames == 0 ? Status::Ok : status;
	}
	s.state.hasPhrase = true;
	s.state.phraseFrames = completion.frames ? completion.frames : s.state.phraseFrames;
	if (play) {
		auto started = audio_.start(slot, settings_.tracks[slot].playback == PlaybackMode::OneShot);
		if (started != Status::Ok) {
			s.state.error = started;
			return started;
		}
		s.state.playing = true;
		advanceStreamGeneration(slot);
		if (dub) {
			auto opened = audio_.setCaptureGate(slot, true);
			if (opened != Status::Ok) {
				s.state.error = opened;
				return opened;
			}
			s.state.capture = true;
		}
	}
	else {
		audio_.stop(slot);
		s.state.playing = false;
	}
	return Status::Ok;
}
Status Engine::execute(SlotId slot, PendingAction action, SampleClock now, const ClockMap& clock) {
	auto& s = slots_[slot];
	switch (action) {
	case PendingAction::Initial: {
		const auto bars = settings_.tracks[slot].recordDurationBars;
		const auto status = audio_.beginInitial(slot, bars ? clock.samplesForBars(bars) : 0);
		if (status != Status::Ok)
			return fail(slot, status);
		s.state.capture = true;
		s.tap = false;
		s.held = false;
		s.initialStart = now;
		s.durationActive = bars != 0;
		if (bars)
			s.duration = durationDeadline(bars, now, clock);
		return Status::Ok;
	}
	case PendingAction::Start: {
		const auto status = audio_.start(slot, settings_.tracks[slot].playback == PlaybackMode::OneShot);
		if (status != Status::Ok)
			return fail(slot, status);
		s.state.playing = true;
		s.state.capture = false;
		advanceStreamGeneration(slot);
		return Status::Ok;
	}
	case PendingAction::FinishInitial:
		return complete(slot, true, settings_.recordAction == RecordAction::REC_DUB_PLAY);
	case PendingAction::ExitInitial:
		return complete(slot, true, false);
	case PendingAction::StopInitial:
		return complete(slot, false, false);
	case PendingAction::Stop:
		close(slot, false);
		audio_.stop(slot);
		s.state.playing = false;
		s.tap = false;
		return Status::Ok;
	case PendingAction::Clear: {
		const auto status = audio_.clear(slot);
		if (status == Status::Busy) {
			queue(slot, action, {true, now + 1, {}});
			return Status::Ok;
		}
		if (status != Status::Ok)
			return fail(slot, status);
		s = {};
		return Status::Ok;
	}
	case PendingAction::None:
		return Status::Ok;
	}
	return Status::Invalid;
}
void Engine::removeSlot(SlotId slot, SampleClock now, const ClockMap& clock) {
	if (!validSlot(slot))
		return;
	close(slot, false);
	audio_.stop(slot);
	slots_[slot] = {};
	if (selected_ == slot)
		selected_ = kInvalidSlot;
	queue(slot, PendingAction::Clear, {true, now, {}});
	service(now, clock);
}
void Engine::service(SampleClock now, const ClockMap& clock) {
	rollback_.advance(now);
	// Bounded global work; clearing never releases a phrase with rollback readers.
	rollback_.service(64);
	for (SlotId id = 0; id < kTrackCount; ++id) {
		auto& s = slots_[id];
		const auto event = audio_.takeCompletion(id);
		if (event.reason == CompletionReason::OneShot) {
			s.state.playing = false;
			s.state.capture = false;
		}
		else if (event.reason != CompletionReason::None && s.state.capture && !s.state.playing) {
			// Preserve the already-consumed completion's length for finishInitial().
			const bool admittedCapacity = event.reason == CompletionReason::Duration && s.durationActive
			                              && s.duration.sampleAt(clock) > s.initialStart + event.frames;
			s.state.phraseFrames = event.frames;
			auto action = s.state.pending ? s.state.action : PendingAction::FinishInitial;
			if (action != PendingAction::StopInitial && action != PendingAction::ExitInitial)
				action = PendingAction::FinishInitial;
			cancelPending(id);
			complete(id, action != PendingAction::StopInitial,
			         action == PendingAction::FinishInitial && settings_.recordAction == RecordAction::REC_DUB_PLAY);
			if (admittedCapacity)
				s.state.error = Status::NoMemory;
			else if (event.reason == CompletionReason::Capacity)
				s.state.error = event.status == Status::Busy ? Status::NoMemory : event.status;
			else if (event.status != Status::Ok)
				s.state.error = event.status;
		}
		const bool userDue = s.state.pending && s.deadline.sampleAt(clock) <= now;
		const bool durationDue = s.durationActive && s.duration.sampleAt(clock) <= now;
		if (durationDue && (!userDue || s.duration.sampleAt(clock) <= s.deadline.sampleAt(clock))) {
			// An explicit lifecycle policy remains authoritative if capacity/duration wins.
			auto action = s.state.pending ? s.state.action : PendingAction::FinishInitial;
			if (action != PendingAction::StopInitial && action != PendingAction::ExitInitial)
				action = PendingAction::FinishInitial;
			cancelPending(id);
			execute(id, action, now, clock);
		}
		else if (userDue) {
			const auto action = s.state.action;
			cancelPending(id);
			execute(id, action, now, clock);
		}
		if (s.tap && (now < s.tapTime || now - s.tapTime >= GestureRollback::kProvisionalSamples))
			s.tap = false;
	}
}
std::optional<SampleClock> Engine::nextActionSample(const ClockMap& clock) const {
	std::optional<SampleClock> next;
	for (const auto& s : slots_) {
		if (s.state.pending) {
			const auto at = s.deadline.sampleAt(clock);
			if (!next || at < *next)
				next = at;
		}
		if (s.durationActive) {
			const auto at = s.duration.sampleAt(clock);
			if (!next || at < *next)
				next = at;
		}
	}
	return next;
}
Status Engine::copyTrackSettings(SlotId from, SlotId to) {
	if (!validSlot(from) || !validSlot(to))
		return Status::Invalid;
	const auto& state = slots_[to].state;
	if (state.capture || state.action == PendingAction::Clear)
		return Status::Busy;
	settings_.tracks[to] = settings_.tracks[from];
	if (state.playing) {
		const bool explicitStop = state.pending && state.action == PendingAction::Stop;
		audio_.setOneShotCompletion(to, settings_.tracks[to].playback == PlaybackMode::OneShot && !explicitStop);
	}
	return Status::Ok;
}
Status Engine::dispatch(Command command, SampleClock now, const ClockMap& clock) {
	service(now, clock); // Expiry and already-due work always precede a new gesture.
	switch (command.type) {
	case CommandType::Configure:
		settings_ = command.configuration;
		for (SlotId id = 0; id < kTrackCount; ++id) {
			const auto& state = slots_[id].state;
			if (state.playing) {
				const bool explicitStop = state.pending && state.action == PendingAction::Stop;
				audio_.setOneShotCompletion(id,
				                            settings_.tracks[id].playback == PlaybackMode::OneShot && !explicitStop);
			}
		}
		return Status::Ok;
	case CommandType::StartAll: {
		const auto deadline = nextDeadline(settings_.startAction, now, clock);
		for (SlotId id = 0; id < kTrackCount; ++id) {
			auto& s = slots_[id];
			if (s.state.hasPhrase && !s.state.playing && !s.state.capture && s.state.error == Status::Ok
			    && !s.state.pending)
				queue(id, PendingAction::Start, deadline);
		}
		service(now, clock);
		return Status::Ok;
	}
	case CommandType::StopAll:
	case CommandType::Exit:
	case CommandType::TransportStop: {
		const auto deadline = nextDeadline(settings_.stopAction, now, clock);
		for (SlotId id = 0; id < kTrackCount; ++id) {
			auto& s = slots_[id];
			const bool initial = s.state.capture && !s.state.playing;
			const bool exit = command.type == CommandType::Exit;
			const bool immediate = command.type == CommandType::TransportStop;
			if (!exit || s.state.action != PendingAction::Stop)
				cancelPending(id);
			s.held = false;
			s.tap = false;
			close(id, false);
			if (initial)
				queue(id,
				      immediate ? PendingAction::StopInitial
				                : (exit ? PendingAction::ExitInitial : PendingAction::StopInitial),
				      immediate ? Deadline{true, now, {}} : deadline);
			else if (s.state.playing && !exit)
				queue(id, PendingAction::Stop, immediate ? Deadline{true, now, {}} : deadline);
		}
		service(now, clock);
		return Status::Ok;
	}
	default:
		break;
	}
	if (!validSlot(command.slot))
		return Status::Invalid;
	const auto id = command.slot;
	auto& s = slots_[id];
	switch (command.type) {
	case CommandType::SetDuration:
		settings_.tracks[id].recordDurationBars = command.durationBars;
		return Status::Ok;
	case CommandType::ShiftPress:
		return Status::Ok;
	case CommandType::Select:
		selected_ = selected_ == id ? kInvalidSlot : id;
		return Status::Ok;
	case CommandType::CaptureError:
		return fail(id, command.status == Status::Ok ? Status::Unavailable : command.status);
	case CommandType::Acknowledge:
		s.state.error = Status::Ok;
		return Status::Ok;
	case CommandType::Release:
		if (!s.held)
			return Status::Ok;
		s.held = false;
		if (!s.state.hasPhrase || s.state.playing || s.state.capture || s.state.pending)
			return Status::Ok;
		if (now >= s.pressTime && now - s.pressTime >= 2 * kSampleRate)
			queue(id, PendingAction::Clear, {true, now, {}});
		else
			queue(id, PendingAction::Start, nextDeadline(settings_.startAction, now, clock));
		break;
	case CommandType::Press:
		if (s.state.error != Status::Ok) {
			s.state.error = Status::Ok;
			s.held = false;
			s.tap = false;
			return Status::Ok;
		}
		if (s.state.pending) {
			cancelPending(id);
			s.tap = false;
			s.held = false;
			return Status::Ok;
		}
		if (!s.state.capture && !s.state.playing && s.state.hasPhrase) {
			s.held = true;
			s.pressTime = now;
			return Status::Ok;
		}
		if (s.state.playing && s.tap && now >= s.tapTime && now - s.tapTime < GestureRollback::kProvisionalSamples) {
			close(id, true);
			s.tap = false;
			queue(id, PendingAction::Stop, nextDeadline(settings_.stopAction, now, clock));
			break;
		}
		if (s.state.capture) {
			if (!s.state.playing)
				queue(id, PendingAction::FinishInitial, nextDeadline(settings_.stopAction, now, clock));
			else {
				close(id, false);
				s.tap = true;
				s.tapTime = now;
			}
		}
		else if (s.state.playing) {
			if (settings_.tracks[id].playback == PlaybackMode::OneShot)
				queue(id, PendingAction::Start, nextDeadline(settings_.startAction, now, clock));
			else {
				const auto status = rollback_.begin(id, now, audio_.position(id), s.state.phraseFrames);
				if (status != Status::Ok)
					return fail(id, status);
				const auto opened = audio_.setCaptureGate(id, true);
				if (opened != Status::Ok) {
					rollback_.cancel(id);
					return fail(id, opened);
				}
				s.state.capture = true;
			}
			s.tap = true;
			s.tapTime = now;
		}
		else
			queue(id, PendingAction::Initial, nextDeadline(settings_.startAction, now, clock));
		break;
	default:
		return Status::Invalid;
	}
	service(now, clock);
	return s.state.error;
}
} // namespace loop_station
