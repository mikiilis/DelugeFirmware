#include "menu_model.h"
#include "page_pool.h"
#include "types.h"
#include <algorithm>
#include <limits>

namespace loop_station {

MenuFields visibleMenuFields(MenuScope scope, const Settings& settings) {
	MenuFields fields;
	auto append = [&](MenuField field) { fields.rows[fields.count++] = field; };
	if (scope == MenuScope::Global) {
		append(MenuField::RecordAction);
		append(MenuField::StartMode);
		if (settings.startAction.mode == ActionMode::Synced)
			append(MenuField::StartInterval);
		append(MenuField::StopMode);
		if (settings.stopAction.mode == ActionMode::Synced)
			append(MenuField::StopInterval);
	}
	else {
		append(MenuField::Source);
		append(MenuField::Monitoring);
		append(MenuField::Duration);
		append(MenuField::Playback);
		append(MenuField::Transpose);
		append(MenuField::Colour);
		append(MenuField::CopyToTrack);
	}
	return fields;
}

uint16_t maxDurationBars(MemoryProfile profile, uint32_t framesPerBar) {
	if (!framesPerBar)
		return 0;
	const size_t frames = activeAudioBytes(profile) / sizeof(Frame) / kTrackCount;
	return static_cast<uint16_t>(std::min<size_t>(frames / framesPerBar, std::numeric_limits<uint16_t>::max()));
}

uint16_t maxDurationBarsForPages(uint32_t freePages, uint32_t framesPerBar) {
	if (!framesPerBar)
		return 0;
	const uint32_t pagesPerTrack = std::min<uint32_t>(freePages / kTrackCount, PagePool::kMaxPagesPerRecord);
	const uint64_t frames = uint64_t(pagesPerTrack) * (kPageBytes / sizeof(Frame));
	const uint64_t barLimit =
	    std::min<uint64_t>(frames / framesPerBar, std::numeric_limits<FrameIndex>::max() / framesPerBar);
	return static_cast<uint16_t>(std::min<uint64_t>(barLimit, std::numeric_limits<uint16_t>::max()));
}

int16_t clampTransposeCents(int32_t cents) {
	return static_cast<int16_t>(std::clamp<int32_t>(cents, -2450, 2450));
}

int16_t stepTransposeCents(int16_t currentCents, int32_t encoderOffset, bool fine) {
	const int64_t next = int64_t(currentCents) + int64_t(encoderOffset) * (fine ? 1 : 100);
	return static_cast<int16_t>(std::clamp<int64_t>(next, -2450, 2450));
}

} // namespace loop_station
