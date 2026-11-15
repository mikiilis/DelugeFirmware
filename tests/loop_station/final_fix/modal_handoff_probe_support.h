#pragma once
#include <array>
#include <cstdint>
#include <vector>

enum class ActionResult { DEALT_WITH, REMIND_ME_OUTSIDE_CARD_ROUTINE };
constexpr int kDisplayWidth = 16, kSideBarWidth = 2, kDisplayHeight = 8;
inline int currentUIMode = 0;
inline bool sdRoutineLock = false;
inline bool stemExportProcessStarted = false;
struct StemExport {
	bool processStarted = false;
};
extern StemExport stemExport;
struct UI {
	virtual ~UI() = default;
	virtual ActionResult padAction(int, int, int) = 0;
	unsigned receivedPresses = 0, receivedReleases = 0;
};
extern UI* currentUI;
inline UI* getCurrentUI() {
	return currentUI;
}
class MatrixDriver {
public:
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity);
	bool padStates[kDisplayWidth + kSideBarWidth][kDisplayHeight]{};
};
struct LoadUI {
	virtual ~LoadUI() = default;
	virtual ActionResult padAction(int, int, int) {
		++baseCalls;
		return ActionResult::DEALT_WITH;
	}
	unsigned baseCalls = 0;
};
class LoadSongUI : public UI, public LoadUI {
public:
	ActionResult padAction(int32_t x, int32_t y, int32_t on) override;
	bool qwertyVisible = true;
	bool qwertyCurrentlyDrawnOnscreen = false;
	int favouriteRow = 1;
	unsigned displayTextCalls = 0;
	void displayText(bool) { ++displayTextCalls; }
};
