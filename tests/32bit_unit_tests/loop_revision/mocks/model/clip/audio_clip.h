#pragma once
#include "model/sample/sample.h"
class AudioClip {
public:
	struct Holder {
		AudioFile* audioFile = nullptr;
		String filePath;
		uint64_t startPos = 0, endPos = 0;
		void setAudioFile(AudioFile* file) {
			if (file)
				file->addReason();
			if (audioFile)
				audioFile->removeReason("test");
			audioFile = file;
		}
		~Holder() { setAudioFile(nullptr); }
	} sampleHolder;
	int32_t loopLength = 96, originalLength = 96;
	uint32_t loopStationSampleZeroTicks = 0, loopStationSampleZeroFrames = 0;
	struct {
		int32_t xScroll = 0;
	} renderData;
	uint32_t loopStationPhaseZeroFrames() const { return loopStationSampleZeroFrames; }
	void unassignVoiceSample(bool) {}
};
