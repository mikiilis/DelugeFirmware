#pragma once
#include "definitions_cxx.hpp"
#include "util/containers.h"
#include "util/d_string.h"
class AudioClip;
class Sample;

// Every Loop Station revision is a complete cumulative WAV. The Sample reason
// held here is intentionally separate from the active AudioClip's reason: old
// revisions must remain available for undo and must be discovered by the normal
// Save Song pass so TEMP recordings are moved to their permanent paths.
struct LoopStationRevision {
	String filePath{};
	Sample* sample = nullptr;
	uint64_t startPos = 0;
	uint64_t endPos = 0;
	int32_t loopLength = 1;
	int32_t originalLength = 1;
	uint32_t sampleZeroTicks = 0;
	uint32_t sampleZeroFrames = UINT32_MAX;

	LoopStationRevision() = default;
	LoopStationRevision(LoopStationRevision const& other);
	LoopStationRevision& operator=(LoopStationRevision const& other);
	LoopStationRevision(LoopStationRevision&& other) noexcept;
	LoopStationRevision& operator=(LoopStationRevision&& other) noexcept;
	~LoopStationRevision();

	void setSample(Sample* newSample);
	void capture(AudioClip const& clip);
	void apply(AudioClip& clip) const;
	Error loadSample(bool mayActuallyReadFile);
};

struct LoopStationRevisionHistory {
	deluge::vector<LoopStationRevision> revisions{};
	// A finished capture is staged here until SampleRecorder confirms that its
	// WAV has been written, finalized, and closed successfully.
	LoopStationRevision provisional{};
	bool provisionalPending = false;
	// -1 is the undoable empty state before the initial take.
	int32_t cursor = -1;

	LoopStationRevisionHistory() = default;
	LoopStationRevisionHistory(LoopStationRevisionHistory const&) = delete;
	LoopStationRevisionHistory& operator=(LoopStationRevisionHistory const&) = delete;

	[[nodiscard]] bool hasCurrent() const;
	[[nodiscard]] bool canUndo() const;
	[[nodiscard]] bool canRedo() const;
	[[nodiscard]] LoopStationRevision const* current() const;
	void clear();
	bool seedFromClip(AudioClip& clip);
	// Reserve all metadata needed by the next successful capture before its
	// recorder starts. This keeps allocation failure on the safe side of commit.
	bool prepareAppend(AudioClip& clip, bool preserveCurrent);
	void appendCurrent(AudioClip& clip);
	bool stageCurrent(AudioClip& clip);
	bool publishStaged(AudioClip& clip);
	void discardStaged();
	Error rejectStaged(AudioClip& clip);
	Error restoreCurrent(AudioClip& clip);
	Error loadSamples(AudioClip* currentClip, bool mayActuallyReadFiles);
	Error navigate(AudioClip& clip, bool redo);
};
