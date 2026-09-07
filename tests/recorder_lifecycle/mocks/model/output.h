#pragma once
#include "model/sample/sample_recorder.h"

// Only the attachment slot is substituted; removeRecorder is the production implementation.
class Output {
public:
	String name;
	SampleRecorder* recorder = nullptr;
	bool removeRecorder(SampleRecorder* expected = nullptr);
};
