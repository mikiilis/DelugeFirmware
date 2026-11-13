#include "probe_support.h"

Song* currentSong = nullptr;
bool sdRoutineLock = false;
ProbeDisplay* display = nullptr;
ProbeUI* probeCurrentUI = nullptr;
unsigned probeRenderRequests = 0;
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
	return true;
}
void changeRootUI(ProbeUI* ui) {
	probeCurrentUI = ui;
}
ProbeUI* getCurrentUI() {
	return probeCurrentUI;
}
void ProbeUI::close() {
	probeCurrentUI = nullptr;
}
void uiNeedsRendering(ProbeUI*) {
	++probeRenderRequests;
}
namespace deluge::l10n {
const char* get(String string) {
	if (string == String::STRING_FOR_LOOP_STATION_TRACK_UNAVAILABLE)
		return "Track unavailable";
	return "Loop setting";
}
} // namespace deluge::l10n
