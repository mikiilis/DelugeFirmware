#pragma once

#include <cstdint>

// Narrow boundaries for the extracted production tempo-encoder dispatch. The
// dispatch body itself is taken verbatim from encoder_input.cpp; only the
// surrounding hardware/UI objects are replaced.

enum class RuntimeFeatureStateToggle { Off, On };
enum class RuntimeFeatureSettingType { Quantize };

namespace deluge::hid::button {
enum Button : uint8_t { TEMPO_ENC = 1 };
} // namespace deluge::hid::button

namespace Buttons {
inline bool tempoEncDown = false;
inline bool shiftDown = false;
inline bool isButtonPressed(deluge::hid::button::Button button) {
	return button == deluge::hid::button::TEMPO_ENC && tempoEncDown;
}
inline bool isShiftButtonPressed() {
	return shiftDown;
}
} // namespace Buttons

struct RuntimeFeatureSettings {
	RuntimeFeatureStateToggle quantize = RuntimeFeatureStateToggle::Off;
	RuntimeFeatureStateToggle get(RuntimeFeatureSettingType type) const {
		return type == RuntimeFeatureSettingType::Quantize ? quantize : RuntimeFeatureStateToggle::Off;
	}
};
inline RuntimeFeatureSettings runtimeFeatureSettings;

// Minimal UI boundary. The real base is a no-op; the probe records calls so the
// dispatcher's notification is observable.
struct UI {
	virtual ~UI() = default;
	unsigned notifications = 0;
	virtual void noteUnrelatedEncoderAction() { ++notifications; }
};
extern UI* probeCurrentUI;
inline UI* getCurrentUI() {
	return probeCurrentUI;
}

struct InstrumentClipView : UI {
	unsigned tempoCalls = 0;
	int8_t lastDelta = 0;
	bool lastButton = false, lastShift = false;
	void tempoEncoderAction(int8_t delta, bool button, bool shift) {
		++tempoCalls;
		lastDelta = delta;
		lastButton = button;
		lastShift = shift;
	}
};
inline InstrumentClipView instrumentClipView{};

struct AutomationView : UI {
	bool noteEditor = false;
	bool inNoteEditor() const { return noteEditor; }
};
inline AutomationView automationView{};

struct PlaybackHandler {
	unsigned tempoCalls = 0;
	int8_t lastDelta = 0;
	bool lastButton = false, lastShift = false;
	void tempoEncoderAction(int8_t delta, bool button, bool shift) {
		++tempoCalls;
		lastDelta = delta;
		lastButton = button;
		lastShift = shift;
	}
};
inline PlaybackHandler playbackHandler{};
