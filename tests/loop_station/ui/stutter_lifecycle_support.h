#pragma once

#include "definitions_cxx.hpp"
#include <array>
#include <cstdint>
#include <limits>

namespace deluge::modulation::params {
inline constexpr int UNPATCHED_STUTTER_RATE = 0;
inline constexpr int GLOBAL_DELAY_RATE = 1;
inline constexpr int kNumParams = 2;
} // namespace deluge::modulation::params

struct StutterConfig {
	bool useSongStutter = true;
	bool quantized = true;
	bool reversed = false;
	bool pingPong = false;
};

struct StutterProbeParam {
	int32_t value = 0;
	void setCurrentValueBasicForSetup(int32_t newValue) { value = newValue; }
};

class UnpatchedParamSet {
public:
	std::array<StutterProbeParam, 8> params{};
	int32_t getValue(int32_t id) { return params[id].value; }
	int32_t paramValueToKnobPos(int32_t value, void*) { return (value + (1 << 24)) >> 25; }
	int32_t knobPosToParamValue(int32_t knobPos, void*) {
		return knobPos < 64 ? knobPos << 25 : std::numeric_limits<int32_t>::max();
	}
};

class ParamManager {
public:
	UnpatchedParamSet* getUnpatchedParamSet() { return &params; }
	UnpatchedParamSet params;
};

class ParamManagerForTimeline : public ParamManager {};

class StutterProbeDelayBuffer {
public:
	Error init(uint32_t rate, uint32_t, bool) {
		lastRate = rate;
		allocated = true;
		return Error::NONE;
	}
	int32_t size() const { return 2048; }
	void discard() { allocated = false; }
	uint32_t lastRate = 0;
	bool allocated = false;
};

class Stutterer {
public:
	int32_t getStutterRate(ParamManager*, int32_t, uint32_t);
	Error beginStutter(void*, ParamManagerForTimeline*, StutterConfig, int32_t, uint32_t);
	void endStutter(ParamManagerForTimeline*);
	bool isStuttering(void* source) const { return stutterSource == source; }

private:
	enum class Status { OFF, RECORDING, PLAYING };
	bool currentReverse = false;
	StutterProbeDelayBuffer buffer;
	Status status = Status::OFF;
	uint8_t sync = 7;
	StutterConfig stutterConfig{};
	int32_t sizeLeftUntilRecordFinished = 0;
	int32_t valueBeforeStuttering = 0;
	int32_t lastQuantizedKnobDiff = 0;
	void* stutterSource = nullptr;
};

class LifecycleModControllableAudio {
public:
	void endStutter(ParamManagerForTimeline* paramManager);
};

class LifecycleView {
public:
	struct ActiveStack {
		void* modControllable = nullptr;
		void* paramManager = nullptr;
	} activeModControllableModelStack;
	bool finishActiveStutterForModTargetChange();
};

extern Stutterer stutterer;
extern LifecycleView targetChangeView;
extern int32_t paramNeutralValues[];
int32_t getFinalParameterValueExp(int32_t neutral, int32_t value);
int32_t cableToExpParamShortcut(int32_t value);
