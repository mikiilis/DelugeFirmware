#include "probe_support.h"

Song* currentSong = nullptr;
bool sdRoutineLock = false;
ProbeDisplay* display = nullptr;
ProbeUI* probeCurrentUI = nullptr;
ProbeUI* probeRootUI = nullptr;
unsigned probeRenderRequests = 0;
std::vector<ProbeUI*> probeRenderTargets;
unsigned probeOledRequests = 0;
uint64_t probeSampleClock = 0;
namespace AudioEngine {
uint64_t loopStationSampleClock() {
	return probeSampleClock;
}
} // namespace AudioEngine
ProbeSessionView sessionView;
namespace Buttons {
bool shiftPressed = false;
}
namespace deluge::gui::menu_item::loop_station {
SettingsMenu loopStationSettingsMenu;
}
bool openUI(ProbeUI* ui) {
	if (!ui->opened())
		return false;
	probeCurrentUI = ui;
	if (!probeRootUI)
		probeRootUI = ui;
	return true;
}
void changeRootUI(ProbeUI* ui) {
	probeCurrentUI = ui;
	probeRootUI = ui;
}
ProbeUI* getCurrentUI() {
	return probeCurrentUI;
}
ProbeUI* getRootUI() {
	return probeRootUI;
}
void ProbeUI::close() {
	probeCurrentUI = this == probeRootUI ? nullptr : probeRootUI;
}
void uiNeedsRendering(ProbeUI* ui) {
	++probeRenderRequests;
	probeRenderTargets.push_back(ui);
}
void renderUIsForOled() {
	++probeOledRequests;
}
namespace deluge::l10n {
const char* get(String string) {
	if (string == String::STRING_FOR_LOOP_STATION_TRACK_UNAVAILABLE)
		return "Track unavailable";
	if (string == String::STRING_FOR_LOOP_STATION_GLOBAL_SETTINGS)
		return "Loop Station Global";
	if (string == String::STRING_FOR_LOOP_STATION_TRACK_SETTINGS)
		return "Loop Station Track";
	if (string == String::STRING_FOR_LOOP_STATION_COPIED)
		return "Copied";
	return "Loop setting";
}
} // namespace deluge::l10n
