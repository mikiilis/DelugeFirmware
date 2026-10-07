/* Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "model/sample/sample_recorder.h"

namespace deluge::loop_station {
// Coordinate ordered generation publication without knowing about the view's
// queue or revision types. All callbacks may yield and re-enter this function.
template <typename Publish, typename Rollback, typename Notify>
void resolveGenerationCommit(bool& slotBusy, bool success, Publish publish, Rollback rollback, Notify notify) {
	if (slotBusy)
		return;
	struct Guard {
		bool& busy;
		Guard(bool& flag) : busy(flag) { busy = true; }
		~Guard() { busy = false; }
	} guard(slotBusy);
	if (success)
		success = publish();
	if (!success)
		rollback();
	notify(success);
}

// All callbacks can yield to audio/card work. The slot stays busy and the parent
// retains commit ownership until history, playback and the engine agree.
template <typename Publish, typename Dependent, typename Quiesce, typename Rollback, typename Notify>
void resolveCaptureCommit(bool& slotBusy, SampleRecorder& parent, bool success, Publish publish, Dependent dependent,
                          Quiesce quiesce, Rollback rollback, Notify notify) {
	if (slotBusy)
		return;
	struct Guard {
		bool& busy;
		Guard(bool& flag) : busy(flag) { busy = true; }
		~Guard() { busy = false; }
	} guard(slotBusy);
	if (success)
		success = publish();
	if (!success) {
		// Obtain this pointer after publication, which may have yielded. Never
		// retain a raw dependent pointer across rollback or other yielding work.
		if (auto* child = dependent()) {
			child->captureOutputInputActive = false;
			child->abort();
		}
		quiesce();
		rollback();
	}
	notify(success);
	// Aborted cleanup can now run, but it must independently wait for source,
	// reader or other references still being released by dependent cleanup.
	parent.releaseCommit(success);
}
} // namespace deluge::loop_station
