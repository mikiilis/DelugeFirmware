#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::abort();
	}
}
bool provisionalMenuOwned = false;
bool physicalPadReleased = false;
} // namespace

int main() {
	using namespace deluge::hid::button;
	Song song;
	SongModel model;
	LoopStationView root;
	song.model = &model;
	uiNavigationHierarchy[0] = &root;
	numUIsOpen = 1;
	lastUIBeforeNullifying = &root;
	currentSong = &song;
	currentUIMode = UI_MODE_NONE;

	// Successful Save takeover keeps its existing behavior: the modal stays current,
	// so the shared-handler tail reconciles the accepted hold.
	check(root.buttonAction(SAVE, true, false) == ActionResult::DEALT_WITH, "real LoopStationView accepts SAVE press");
	check(currentUIMode == UI_MODE_HOLDING_SAVE_BUTTON, "SAVE press starts normal shared hold");
	check(model.held && model.cancellations == 0, "hold remains until Save UI takes over");
	check(root.buttonAction(SAVE, false, true) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE,
	      "Save release defers in card routine");
	check(getCurrentUI() == &root && model.held && model.cancellations == 0,
	      "deferred Save release has no premature takeover");
	check(root.buttonAction(SAVE, false, false) == ActionResult::DEALT_WITH,
	      "real root Save release retry opens shared UI");
	check(getCurrentUI() == &saveSongUI, "actual Save handler takes current UI ownership");
	check(!model.held && model.cancellations == 1, "Loop Station cancels accepted hold on Save takeover");

	// Successful Load takeover.
	uiNavigationHierarchy[0] = &root;
	numUIsOpen = 1;
	model.held = true;
	model.cancellations = 0;
	currentUIMode = UI_MODE_NONE;
	check(root.buttonAction(LOAD, true, false) == ActionResult::DEALT_WITH, "real LoopStationView accepts LOAD press");
	check(root.buttonAction(LOAD, false, false) == ActionResult::DEALT_WITH, "real root Load release opens shared UI");
	check(getCurrentUI() == &loadSongUI, "actual Load handler takes current UI ownership");
	check(!model.held && model.cancellations == 1, "Loop Station cancels accepted hold on Load takeover");

	// Failed Save opening: production openUI makes the modal current before opened(),
	// the modal consumes the accepted centre release during that I/O, then opening
	// fails and openUI restores the root. The focus regain must clear the stale hold.
	uiNavigationHierarchy[0] = &root;
	numUIsOpen = 1;
	model.held = true;
	model.cancellations = 0;
	root.refreshed = false;
	root.cancellationsAtRefresh = 0;
	saveSongUI.releases = 0;
	provisionalMenuOwned = false;
	physicalPadReleased = false;
	saveSongUI.openedCallback = [](SaveSongUI& self) -> bool {
		provisionalMenuOwned = (getCurrentUI() == &self);
		matrixDriver.padAction(2, 3, 0);
		physicalPadReleased = (matrixDriver.padStates[2][3] == 0);
		return false;
	};
	currentUIMode = UI_MODE_NONE;
	check(root.buttonAction(SAVE, true, false) == ActionResult::DEALT_WITH, "failed Save: press accepted");
	check(root.buttonAction(SAVE, false, false) == ActionResult::DEALT_WITH,
	      "failed Save: release opens then rolls back");
	saveSongUI.openedCallback = nullptr;
	check(provisionalMenuOwned, "Save modal owns the release provisionally");
	check(physicalPadReleased, "physical matrix records the modal-time release");
	check(saveSongUI.releases == 1, "Save modal consumed the release");
	check(getCurrentUI() == &root, "failed Save open restores the root");
	check(!model.held && model.cancellations == 1, "failed Save open cancels the stale accepted hold");
	check(root.refreshed && root.cancellationsAtRefresh == 1, "Save cancellation precedes the ordinary refresh");

	// A still-held gesture's later physical release reaches the restored root; it
	// must not be replayed to the modal, re-cancel, or duplicate any action.
	const unsigned cancellationsBefore = model.cancellations;
	check(matrixDriver.padAction(2, 3, 100) == ActionResult::DEALT_WITH, "later root press is handled");
	check(matrixDriver.padAction(2, 3, 0) == ActionResult::DEALT_WITH, "later root release is handled");
	check(root.padPresses == 1 && root.padReleases == 1, "later gesture reaches the restored root");
	check(saveSongUI.releases == 1, "no delayed release is replayed to the modal");
	check(model.cancellations == cancellationsBefore, "later releases do not re-run cancellation");
	check(!model.held, "later releases leave the hold cleared");

	// Failed Load opening, consuming the release through the real LoadSongUI body.
	uiNavigationHierarchy[0] = &root;
	numUIsOpen = 1;
	model.held = true;
	model.cancellations = 0;
	root.refreshed = false;
	root.cancellationsAtRefresh = 0;
	loadSongUI.baseCalls = 0;
	provisionalMenuOwned = false;
	loadSongUI.openedCallback = [](LoadSongUI& self) -> bool {
		provisionalMenuOwned = (getCurrentUI() == &self);
		matrixDriver.padAction(4, 5, 0);
		return false;
	};
	currentUIMode = UI_MODE_NONE;
	check(root.buttonAction(LOAD, true, false) == ActionResult::DEALT_WITH, "failed Load: press accepted");
	check(root.buttonAction(LOAD, false, false) == ActionResult::DEALT_WITH,
	      "failed Load: release opens then rolls back");
	loadSongUI.openedCallback = nullptr;
	check(provisionalMenuOwned, "Load modal owns the release provisionally");
	check(loadSongUI.baseCalls == 1, "real LoadSongUI::padAction consumes the modal-time release");
	check(getCurrentUI() == &root, "failed Load open restores the root");
	check(!model.held && model.cancellations == 1, "failed Load open cancels the stale accepted hold");
	check(root.refreshed && root.cancellationsAtRefresh == 1, "Load cancellation precedes the ordinary refresh");

	// A replaced current Song must be reconciled through the current active model,
	// never through a saved owner pointer.
	SongModel replacement;
	Song replacementSong;
	replacementSong.model = &replacement;
	currentSong = &replacementSong;
	model.held = true;
	model.cancellations = 0;
	replacement.held = true;
	replacement.cancellations = 0;
	root.focusRegained();
	check(replacement.cancellations == 1 && !replacement.held, "focus regain reconciles the replaced current model");
	check(model.cancellations == 0 && model.held, "stale model is left untouched");

	// A null current Song must not crash the focus regain.
	currentSong = nullptr;
	const unsigned cancellationsAtNull = model.cancellations;
	root.focusRegained();
	check(getCurrentUI() == &root, "null current Song focus regain leaves the hierarchy intact");
	check(model.cancellations == cancellationsAtNull, "null current Song focus regain cancels nothing");
	currentSong = &song;
	return 0;
}
