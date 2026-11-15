#include "settings_probe_support.h"

Song* currentSong = nullptr;
bool sdRoutineLock = false;
ProbeDisplay* display = nullptr;
ProbeUI* probeCurrentUI = nullptr;
ProbeUI* probeRootUI = nullptr;
unsigned probeRenderRequests = 0;
std::vector<ProbeUI*> probeRenderTargets;
std::vector<ProbePadFrame> probeSentPadFrames;
std::vector<std::pair<uint32_t, uint32_t>> probeRenderRows;
unsigned probeOledRequests = 0;
uint64_t probeSampleClock = 0;
namespace AudioEngine {
uint64_t loopStationSampleClock() {
	return probeSampleClock;
}
} // namespace AudioEngine
ProbeSessionView sessionView;
uint32_t currentUIMode = 0;
ProbeView view;
LifecycleView targetChangeView;
namespace Buttons {
bool shiftPressed = false;
}
namespace deluge::gui::menu_item::loop_station {
SettingsMenu loopStationSettingsMenu;
}
namespace production_dispatch {
void setHierarchy(ProbeUI* root, ProbeUI* current);
void uiNeedsRendering(ProbeUI* ui, uint32_t mainRows, uint32_t sideRows);
void dispatchTick(bool ready);
} // namespace production_dispatch
static bool probeProductionDispatch = false;
bool openUI(ProbeUI* ui) {
	if (!ui->opened()) {
		if (probeCurrentUI)
			probeCurrentUI->focusRegained();
		return false;
	}
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
	if (probeCurrentUI)
		probeCurrentUI->focusRegained();
}
void ProbeUI::graphicsRoutine() {
	if (probeRootUI && probeRootUI != this)
		probeRootUI->graphicsRoutine();
}
void probeDispatchGraphicsTick(bool uartReady) {
	if (probeRootUI && probeCurrentUI) {
		probeProductionDispatch = true;
		production_dispatch::setHierarchy(probeRootUI, probeCurrentUI);
		production_dispatch::dispatchTick(uartReady);
		probeProductionDispatch = false;
	}
}
void uiNeedsRendering(ProbeUI* ui, uint32_t mainRows, uint32_t sideRows) {
	++probeRenderRequests;
	probeRenderTargets.push_back(ui);
	probeRenderRows.emplace_back(mainRows, sideRows);
	if (probeProductionDispatch)
		production_dispatch::uiNeedsRendering(ui, mainRows, sideRows);
}
void renderUIsForOled() {
	++probeOledRequests;
}
namespace deluge::l10n {
const char* get(String string) {
	if (auto it = probeTranslations.find(string); it != probeTranslations.end())
		return it->second.c_str();
	switch (string) {
#include "english_cases.h"
	default:
		break;
	}
	return "Loop setting";
}
} // namespace deluge::l10n
