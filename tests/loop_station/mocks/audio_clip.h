#pragma once

#include <string>

namespace loop_station {
class AudioAdapter;
}

class AudioClip {
public:
	std::string name;
	unsigned output = 0, fx = 99;
	loop_station::AudioAdapter* adapter = nullptr;
};
