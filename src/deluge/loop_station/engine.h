#pragma once
#include "config.h"
#include "gesture_rollback.h"
#include "resident_audio.h"
#include "timing.h"
#include <optional>
namespace loop_station {
enum class CommandType : uint8_t {
	Press,
	Release,
	ShiftPress,
	Select,
	StartAll,
	StopAll,
	Exit,
	TransportStop,
	CaptureError,
	Acknowledge,
	SetDuration,
	Configure
};
struct Command {
	CommandType type;
	SlotId slot = 0;
	Status status = Status::Ok;
	uint16_t durationBars = 0;
	Settings configuration = defaultSettings();
	static Command configure(Settings settings) {
		Command c{CommandType::Configure};
		c.configuration = settings;
		return c;
	}
};
enum class PendingAction : uint8_t { None, Initial, Start, FinishInitial, Stop, ExitInitial, StopInitial, Clear };
struct SlotState {
	bool hasPhrase = false, capture = false, playing = false, pending = false;
	Status error = Status::Ok;
	FrameIndex phraseFrames = 0;
	PendingAction action = PendingAction::None;
};
class Engine {
public:
	Engine(ResidentAudio& audio, GestureRollback& rollback) : audio_(audio), rollback_(rollback) {}
	Status dispatch(Command command, SampleClock now, const ClockMap& clock);
	Status copyTrackSettings(SlotId from, SlotId to);
	// Serialized sample-boundary owner: call before selecting each render window.
	void service(SampleClock now, const ClockMap& clock);
	// Detach a deleted/unbound slot immediately; bounded rollback retirement may defer clear.
	void removeSlot(SlotId slot, SampleClock now, const ClockMap& clock);
	void phraseReplaced(SlotId slot, FrameIndex length, bool playing);
	std::optional<SampleClock> nextActionSample(const ClockMap& clock) const;
	SlotState state(SlotId slot) const { return validSlot(slot) ? slots_[slot].state : SlotState{}; }
	SampleClock heldDurationSamples(SlotId slot, SampleClock now) const {
		if (!validSlot(slot) || !slots_[slot].held || now < slots_[slot].pressTime)
			return 0;
		return now - slots_[slot].pressTime;
	}
	// Changes whenever a slot's playback stream is restarted or its phrase is replaced.
	uint32_t streamGeneration(SlotId slot) const { return validSlot(slot) ? streamGeneration_[slot] : 0; }
	const Settings& settings() const { return settings_; }
	SlotId selected() const { return selected_; }

private:
	struct Slot {
		SlotState state{};
		Deadline deadline{}, duration{};
		SampleClock pressTime = 0, tapTime = 0, initialStart = 0;
		bool durationActive = false, held = false, tap = false;
	};
	Status execute(SlotId slot, PendingAction action, SampleClock now, const ClockMap& clock);
	void queue(SlotId slot, PendingAction action, Deadline deadline);
	Status complete(SlotId slot, bool play, bool dub);
	Status fail(SlotId slot, Status status);
	void close(SlotId slot, bool cancel);
	void cancelPending(SlotId slot);
	void advanceStreamGeneration(SlotId slot);
	ResidentAudio& audio_;
	GestureRollback& rollback_;
	Settings settings_ = defaultSettings();
	Slot slots_[kTrackCount]{};
	uint32_t streamGeneration_[kTrackCount]{};
	SlotId selected_ = kInvalidSlot;
};
} // namespace loop_station
