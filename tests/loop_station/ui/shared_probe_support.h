#pragma once
#include "hid/encoder_acceleration.h"
#include "probe_support.h"
#include <cassert>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <span>

using ModControllableAudio = void;

constexpr int kShortStringBufferSize = 100;
enum class PopupType { NOTIFICATION };
namespace FlashStorage {
inline MenuHighlighting accessibilityMenuHighlighting = MenuHighlighting::FULL_INVERSION;
}
enum Feature { HorizontalMenus };
struct ProbeRuntimeFeatureSettings {
	bool horizontal = true;
	bool isOn(Feature) const { return horizontal; }
};
inline ProbeRuntimeFeatureSettings runtimeFeatureSettings;
inline double probeMenuTime = 1;
inline double getSystemTime() {
	return probeMenuTime;
}
namespace deluge {
template <class T>
using vector = std::vector<T>;
}
namespace util {
template <class T, class... U>
bool one_of(T v, U... values) {
	return ((v == values) || ...);
}
template <class T>
bool one_of(T v, std::initializer_list<T> values) {
	return std::ranges::find(values, v) != values.end();
}
} // namespace util
class StringBuf {
public:
	std::string value;
	void append(std::string_view text) { value += text; }
	void appendInt(int n) { value += std::to_string(n); }
	void appendFloat(float n, int, int digits) {
		char s[32];
		std::snprintf(s, sizeof(s), "%.*f", digits, n);
		value += s;
	}
	void clear() { value.clear(); }
	const char* c_str() const { return value.c_str(); }
	const char* data() const { return value.c_str(); }
	void removeSpaces() { std::erase(value, ' '); }
	void truncate(size_t size) { value.resize(size); }
	size_t size() const { return value.size(); }
};
#define DEF_STACK_STRING_BUF(name, size) StringBuf name
namespace deluge::hid::display {
struct OLED {
	static inline oled_canvas::Canvas main;
	static void markChanged() {}
};
} // namespace deluge::hid::display
namespace deluge::modulation::params {
enum class Kind { NONE };
}
struct MIDICable;
struct PatchingSourceShortcutTarget {};
class MultiRange {};
namespace deluge::gui::menu_item {
class Submenu;
class HorizontalMenu;
enum class MenuPermission { YES, NO, MUST_SELECT_RANGE };
class MenuItem {
public:
	struct SlotPosition {
		uint8_t start_x, start_y, width, height;
	};
	Submenu* parent = nullptr;
	bool relevant = true;
	int slots = 1, edits = 0, presses = 0, permissions = 0, reads = 0;
	bool range = false, enter = false, submenu = false;
	MenuPermission permission = MenuPermission::YES;
	void* permissionMod = nullptr;
	int permissionSource = -1;
	mutable int relevanceCalls = 0;
	MenuItem(l10n::String = {}, l10n::String = {}) {}
	l10n::String title{};
	virtual ~MenuItem() = default;
	virtual void beginSession(MenuItem*) {}
	virtual void endSession();
	virtual bool isRelevant(void*, int) const {
		++relevanceCalls;
		return relevant;
	}
	virtual bool isRangeDependent() { return range; }
	virtual void selectEncoderAction(int32_t offset) { edits += offset; }
	virtual void horizontalEncoderAction(int32_t) {}
	virtual MenuItem* selectButtonPress() {
		++presses;
		return nullptr;
	}
	virtual ActionResult buttonAction(hid::Button, bool, bool) { return ActionResult::NOT_DEALT_WITH; }
	virtual void renderOLED() {}
	virtual void readValueAgain() {}
	virtual void unlearnAction() {}
	virtual bool usesAffectEntire() { return false; }
	virtual bool allowsLearnMode() { return false; }
	virtual void learnKnob(MIDICable*, int32_t, int32_t, int32_t) {}
	virtual void learnProgramChange(MIDICable&, int32_t, int32_t) {}
	virtual bool learnNoteOn(MIDICable&, int32_t, int32_t) { return false; }
	virtual void renderInHorizontalMenu(const SlotPosition&) {}
	virtual void drawPixelsForOled() {}
	virtual bool isSubmenu() { return submenu; }
	virtual void updatePadLights() {}
	virtual MenuItem* patchingSourceShortcutPress(PatchSource, bool) { return nullptr; }
	virtual PatchingSourceShortcutTarget getPatchingSourceShortcutTarget(bool) const { return {}; }
	virtual modulation::params::Kind getParamKind() { return modulation::params::Kind::NONE; }
	virtual uint32_t getParamIndex() { return 0; }
	virtual int32_t getOccupiedSlots() const { return slots; }
	virtual bool showNotification() const { return true; }
	virtual bool selectEncoderActionEditsInstrument() { return false; }
	bool selectEncoderActionIsPermitted() { return true; }
	bool allowToBeginSessionFromHorizontalMenu() { return enter; }
	bool shouldEnterSubmenu() { return true; }
	virtual MenuPermission checkPermissionToBeginSession(void* mod, int source, MultiRange**) {
		++permissions;
		permissionMod = mod;
		permissionSource = source;
		return permission;
	}
	virtual void readCurrentValue() { ++reads; }
	bool showColumnLabel() { return true; }
	void getColumnLabel(StringBuf& s) { s.value = "Item"; }
	virtual void getNotificationValue(StringBuf& s) { s.value = "value"; }
	const char* getName() const { return "Item"; }
	const char* getTitle() const { return "Menu"; }
	void drawName() {}
};
#define NO_NAVIGATION ((MenuItem*)0xFFFFFFFF)
} // namespace deluge::gui::menu_item
struct ProbeSoundEditor {
	MultiRange* currentMultiRange = nullptr;
	int16_t currentMultiRangeIndex = 0;
	void* currentModControllable = nullptr;
	void* currentClip = nullptr;
	int currentSourceIndex = 0, navigationDepth = 0;
	int numberEditPos = 0, numberEditSize = 0, numberScrollAmount = 0;
	int currentParamShortcutX = 0, currentParamShortcutY = 0, entries = 0, exits = 0, padUpdates = 0;
	deluge::gui::menu_item::MenuItem* menuItemNavigationRecord[16]{};
	std::optional<std::span<deluge::gui::menu_item::HorizontalMenu* const>> getCurrentHorizontalMenusChain();
	void enterSubmenu(deluge::gui::menu_item::MenuItem* item) {
		++entries;
		menuItemNavigationRecord[++navigationDepth] = item;
	}
	void goUpOneLevel() {
		++exits;
		--navigationDepth;
	}
	void updatePadLightsFor(deluge::gui::menu_item::MenuItem*) {
		++padUpdates;
		currentParamShortcutX = 0;
	}
};
inline ProbeSoundEditor soundEditor;
#include "shared_menu_declarations.h"
namespace deluge::gui::menu_item {
bool isItemRelevant(MenuItem* item);
inline void Submenu::drawPixelsForOled() {
}
inline void Submenu::renderInHorizontalMenu(const SlotPosition&) {
}
inline void Submenu::unlearnAction() {
}
inline bool Submenu::usesAffectEntire() {
	return false;
}
inline bool Submenu::allowsLearnMode() {
	return false;
}
inline void Submenu::learnKnob(MIDICable*, int32_t, int32_t, int32_t) {
}
inline void Submenu::learnProgramChange(MIDICable&, int32_t, int32_t) {
}
inline bool Submenu::learnNoteOn(MIDICable&, int32_t, int32_t) {
	return false;
}
inline MenuItem* Submenu::patchingSourceShortcutPress(PatchSource, bool) {
	return nullptr;
}
inline PatchingSourceShortcutTarget Submenu::getPatchingSourceShortcutTarget(bool) const {
	return {};
}
struct ProbeMultiRangeMenu : MenuItem {
	MenuItem* menuItemHeadingTo = nullptr;
};
inline ProbeMultiRangeMenu multiRangeMenu;
inline HorizontalMenu modFXMenu{{}, {}}, globalModFXMenu{{}, {}};
class HorizontalMenuContainer {
public:
	std::span<MenuItem*> getItems() { return {}; }
	int getOccupiedSlotsCount() { return 1; }
	void render(MenuItem::SlotPosition, const MenuItem*, HorizontalMenu*) {}
};
inline std::vector<HorizontalMenuContainer*> horizontalMenuContainers;
template <typename T>
class Value : public MenuItem {
public:
	using MenuItem::MenuItem;
	void selectEncoderAction(int32_t) override;
	virtual void writeCurrentValue() = 0;
	virtual void drawValue() {}
};
class Number : public Value<int32_t> {
public:
	using Value::Value;
	int32_t value = 123, written = 0;
	int32_t getValue() { return value; }
	void setValue(int32_t n) { value = n; }
	virtual int32_t getMinValue() const { return -2450; }
	virtual int32_t getMaxValue() const { return 2450; }
	void writeCurrentValue() override { written = value; }
	virtual int getRenderingStyle() const { return 0; }
};
using RenderingStyle = int;
inline constexpr int NUMBER = 0;
class Decimal : public Number {
public:
	using Number::Number;
	virtual int32_t getNumberEditSize();
	virtual int32_t getNumDecimalPlaces() const { return 2; }
	void getNotificationValue(StringBuf& value) override { value.appendFloat(getValue() / 100.f, 2, 2); }
	void selectEncoderAction(int32_t) override;
	void scrollToGoodPos();
	void renderInHorizontalMenu(const SlotPosition&) override;
};
class Integer : public Number {
public:
	using Number::Number;
};
class Selection : public Value<int32_t> {
public:
	using Value::Value;
	int32_t value = 0;
	void setValue(int32_t v) { value = v; }
	int32_t getValue() { return value; }
	enum class OptType { FULL, SHORT };
	virtual deluge::vector<std::string_view> getOptions(OptType = OptType::FULL) = 0;
};
} // namespace deluge::gui::menu_item
inline std::vector<deluge::gui::menu_item::HorizontalMenu*> probeChain;
inline std::optional<std::span<deluge::gui::menu_item::HorizontalMenu* const>>
ProbeSoundEditor::getCurrentHorizontalMenusChain() {
	if (probeChain.empty())
		return std::nullopt;
	return std::span<deluge::gui::menu_item::HorizontalMenu* const>(probeChain);
}
inline int getNumDecimalDigits(int n) {
	return n ? static_cast<int>(std::log10(n)) + 1 : 1;
}
