#pragma once
#include "definitions_cxx.hpp"
#include "model/sample/sample.h"
struct AudioFileManager {
	AudioFile* getAudioFileFromFilename(String&, bool, Error* error, void*, AudioFileType) {
		*error = Error::FILE_NOT_FOUND;
		return nullptr;
	}
};
inline AudioFileManager audioFileManager;
