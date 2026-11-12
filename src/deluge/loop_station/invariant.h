#pragma once
// Firmware has no newlib stdio/syscalls. Keep loop invariants on its existing
// fault path; host tests retain ordinary assertions. Set only by firmware CMake.
#if LOOP_STATION_FIRMWARE
#include "definitions.h"
#else
#include <cassert>
#endif
namespace loop_station::detail {
inline void invariant(bool condition) {
#ifndef NDEBUG
#if LOOP_STATION_FIRMWARE
	if (!condition)
		FREEZE_WITH_ERROR("LOOP");
#else
	assert(condition);
#endif
#else
	(void)condition;
#endif
}
} // namespace loop_station::detail
