#pragma once
#include "definitions_cxx.hpp"
#include <functional>

namespace recorder_test {
enum class Failure { NONE, SAMPLE_ALLOCATION, SAMPLE_INITIALIZE, FIRST_CLUSTER };
inline Failure failure = Failure::NONE;
inline int liveSamples = 0;
inline int liveAllocations = 0;
inline int clusterReleases = 0;
inline int fileCloses = 0;
inline int unlinks = 0;
inline bool fileOpen = false;
inline std::function<void()> onClose;
inline std::function<void()> onCreate;
inline Error createError = Error::NONE;
inline Error writeError = Error::NONE;
inline Error finalizeError = Error::NONE;
inline int liveClusters = 0;
inline int destroyedClusters = 0;
inline int seeks = 0;
inline int truncations = 0;
inline int headerWrites = 0;
inline bool seekFails = false;
inline bool truncateFails = false;
inline bool closeFails = false;
inline bool headerFails = false;
inline bool clusterLoadFails = false;
inline bool clusterInsertFails = false;
inline bool recycleReleasedClusters = false;
inline std::function<void()> onSeek;
inline std::function<void()> onTruncate;
inline std::function<void()> onWrite;
inline uint32_t fileSize = 0;
inline uint32_t filePosition = 0;
} // namespace recorder_test
