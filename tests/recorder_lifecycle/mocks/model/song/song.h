#pragma once
#include "util/d_string.h"
struct Song {
	String name;
};
inline Song testSong;
inline Song* currentSong = &testSong;
