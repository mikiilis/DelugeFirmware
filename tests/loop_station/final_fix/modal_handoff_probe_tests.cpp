#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::abort();
	}
}
struct LoopStationRoot : UI {
	ActionResult padAction(int, int, int velocity) override {
		if (velocity)
			++receivedPresses;
		else
			++receivedReleases;
		return ActionResult::DEALT_WITH;
	}
};
} // namespace
int main() {
	MatrixDriver matrix;
	LoadSongUI load;
	LoopStationRoot oldRoot;
	LoopStationRoot replacementRoot;
	currentUI = &oldRoot;
	check(matrix.padAction(2, 3, 100) == ActionResult::DEALT_WITH, "Loop Station root accepts initial hold");
	check(oldRoot.receivedPresses == 1, "old root receives accepted initial press");
	currentUI = &load;
	load.qwertyVisible = true;
	check(matrix.padAction(2, 3, 0) == ActionResult::DEALT_WITH, "menu owns physical pad release");
	check(matrix.padStates[2][3] == 0, "physical matrix state records release");
	check(load.baseCalls == 1, "actual LoadSongUI handler owns the physical release");
	check(oldRoot.receivedPresses == 1 && oldRoot.receivedReleases == 0,
	      "old root receives its accepted press but not the modal release");
	check(matrix.padAction(2, 3, 100) == ActionResult::DEALT_WITH, "new physical press belongs to Load menu");
	check(matrix.padAction(2, 3, 0) == ActionResult::DEALT_WITH, "new menu press release stays menu-owned");
	check(load.baseCalls == 3, "new menu gesture edges are handled by actual LoadSongUI base path");

	// Replacing the owner while the modal is active cannot replay a stale pad
	// release to either the previous or newly installed root.
	currentUI = &replacementRoot;
	check(oldRoot.receivedReleases == 0 && replacementRoot.receivedReleases == 0,
	      "owner replacement receives no delayed release");
	return 0;
}
