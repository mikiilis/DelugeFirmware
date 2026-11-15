#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::abort();
	}
}
} // namespace
int main() {
	using namespace deluge::hid::button;
	Song song;
	SongModel model;
	LoopStationView root;
	song.model = &model;
	rootUI = currentUI = &root;
	currentSong = &song;
	currentUIMode = UI_MODE_NONE;

	check(root.buttonAction(SAVE, true, false) == ActionResult::DEALT_WITH, "real LoopStationView accepts SAVE press");
	check(currentUIMode == UI_MODE_HOLDING_SAVE_BUTTON, "SAVE press starts normal shared hold");
	check(model.held && model.cancellations == 0, "hold remains until Save UI takes over");
	check(root.buttonAction(SAVE, false, true) == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE,
	      "Save release defers in card routine");
	check(currentUI == &root && model.held && model.cancellations == 0,
	      "deferred Save release has no premature takeover");
	check(root.buttonAction(SAVE, false, false) == ActionResult::DEALT_WITH,
	      "real root Save release retry opens shared UI");
	check(currentUI == &saveSongUI, "actual Save handler takes current UI ownership");
	check(!model.held && model.cancellations == 1, "Loop Station cancels accepted hold on Save takeover");

	currentUI = &root;
	model.held = true;
	currentUIMode = UI_MODE_NONE;
	check(root.buttonAction(LOAD, true, false) == ActionResult::DEALT_WITH, "real LoopStationView accepts LOAD press");
	check(root.buttonAction(LOAD, false, false) == ActionResult::DEALT_WITH, "real root Load release opens shared UI");
	check(currentUI == &loadSongUI, "actual Load handler takes current UI ownership");
	check(!model.held && model.cancellations == 2, "Loop Station cancels accepted hold on Load takeover");
	return 0;
}
