#pragma once
#include "util/d_string.h"
struct StemExport {
	bool processStarted = false;
	Error getUnusedStemRecordingFilePath(String*, AudioRecordingFolder) { return Error::NONE; }
};
inline StemExport stemExport;
