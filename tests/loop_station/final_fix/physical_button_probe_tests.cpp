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
	UI root;
	SettingsMenu settings;
	rootUI = currentUI = &root;
	getCurrentUICalls = 0;
	view.pendingStutterEncoderRelease_ = true;
	currentUI = &settings;
	const auto canceled = Buttons::buttonAction(MOD_ENCODER_1, false, false);
	check(canceled == ActionResult::DEALT_WITH, "owned canceled physical release is consumed");
	check(!view.pendingStutterEncoderRelease_, "physical release clears pending ownership");
	check(!Buttons::isButtonPressed(MOD_ENCODER_1), "physical release updates button bookkeeping first");
	check(getCurrentUICalls == 0, "canceled release returns before current-UI handler lookup");

	settings.closeMenu();
	check(currentUI == &root, "settings close returns to root UI");
	Buttons::buttonAction(MOD_ENCODER_1, true, false);
	Buttons::buttonAction(MOD_ENCODER_1, false, false);
	check(root.encoderEdges.size() == 2, "fresh physical gesture has exactly press and release callbacks");
	check(root.encoderEdges[0].first && !root.encoderEdges[1].first, "fresh release is not consumed by stale latch");
	check(!view.pendingStutterEncoderRelease_, "fresh gesture leaves no stale ownership");

	currentUI = &settings;
	sdRoutineLock = true;
	const auto deferred = Buttons::buttonAction(SELECT_ENC, true, true);
	check(deferred == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE, "real settings handler defers card press");
	check(Buttons::isButtonPressed(SELECT_ENC), "deferred press bookkeeping is retained");
	sdRoutineLock = false;
	const auto retried = Buttons::buttonAction(SELECT_ENC, true, false);
	check(retried == ActionResult::DEALT_WITH, "settings press retry handles event outside card routine");
	check(settings.selectHeld_, "settings owns retried SELECT press");
	const auto released = Buttons::buttonAction(SELECT_ENC, false, false);
	check(released == ActionResult::DEALT_WITH, "real settings handler consumes SELECT release");
	check(!settings.selectHeld_, "settings release clears its physical press latch");
	check(!Buttons::isButtonPressed(SELECT_ENC), "settings release clears physical bookkeeping");
	return 0;
}
