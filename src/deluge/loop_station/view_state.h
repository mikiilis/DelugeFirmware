#pragma once

#include "engine.h"

namespace loop_station {

enum class PendingDestination : uint8_t { None, Capture, Overdub, Play, Stop, Delete };

constexpr PendingDestination pendingDestination(PendingAction action, RecordAction recordAction) {
	switch (action) {
	case PendingAction::Initial:
		return PendingDestination::Capture;
	case PendingAction::Start:
		return PendingDestination::Play;
	case PendingAction::FinishInitial:
		return recordAction == RecordAction::REC_PLAY_DUB ? PendingDestination::Play : PendingDestination::Overdub;
	case PendingAction::Stop:
	case PendingAction::StopInitial:
		return PendingDestination::Stop;
	case PendingAction::ExitInitial:
		return PendingDestination::Play;
	case PendingAction::Clear:
		return PendingDestination::Delete;
	case PendingAction::None:
		return PendingDestination::None;
	}
	return PendingDestination::None;
}

} // namespace loop_station
