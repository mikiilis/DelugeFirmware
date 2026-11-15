#pragma once

#include "definitions_cxx.hpp"

namespace deluge::gui::views {

template <typename SetLed>
inline void setAffectEntireLedState(bool clipContext, UIType uiContextType, bool rootAffectEntire,
                                    bool instrumentClipAffectEntire, SetLed&& setLed) {
	const bool affectEntire = !clipContext || uiContextType == UIType::LOOP_STATION ? rootAffectEntire
	                          : uiContextType == UIType::INSTRUMENT_CLIP            ? instrumentClipAffectEntire
	                                                                                : true;
	setLed(affectEntire);
}

} // namespace deluge::gui::views
