/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "gui/menu_item/submenu.h"
#include "loop_station/loop_station.h"

namespace deluge::gui::menu_item::loop_station {

extern Submenu globalSettingsMenu;
extern Submenu trackSettingsMenu;

void selectTrack(deluge::loop_station::SlotId slot);
[[nodiscard]] deluge::loop_station::SlotId selectedTrack();

} // namespace deluge::gui::menu_item::loop_station
