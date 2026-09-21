#pragma once
#include "util/d_string.h"
#include <cassert>
class AudioFile {
public:
	String filePath;
	int reasons = 0;
	void addReason() { ++reasons; }
	void removeReason(char const*) {
		assert(reasons > 0);
		--reasons;
	}
};
class Sample : public AudioFile {};
