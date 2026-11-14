#pragma once

#include "deluge/loop_station/menu_model.h"
#include "gui/menu_item/decimal.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/selection.h"
#include <array>
#include <span>

namespace loop_station {
class SongModel;
}

namespace deluge::gui::menu_item::loop_station {
namespace ls = ::loop_station;
const char* compactSettingLabel(ls::MenuField field);

// Resolves the current song lazily. The host owns allocation, binding and failure feedback.
struct SettingsContext {
	ls::MenuScope scope = ls::MenuScope::Global;
	ls::SlotId slot = 0;
	ls::SongModel* model() const;
	bool relevant(ls::MenuField field) const;
	int32_t read(ls::MenuField field) const;
	ls::Status write(ls::MenuField field, int32_t value);
	uint16_t maxDurationBars() const;
};

class EnumSetting final : public Selection {
public:
	EnumSetting(SettingsContext& context, ls::MenuField field, l10n::String name)
	    : Selection(name), context_(context), field_(field) {}
	bool isRelevant(ModControllableAudio*, int32_t) const override { return context_.relevant(field_); }
	void beginSession(MenuItem* = nullptr) override { readCurrentValue(); }
	void readCurrentValue() override { setValue(context_.read(field_)); }
	void writeCurrentValue() override;
	void selectEncoderAction(int32_t offset) override;
	MenuItem* selectButtonPress() override { return NO_NAVIGATION; }
	deluge::vector<std::string_view> getOptions(OptType = OptType::FULL) override;
	void getNotificationValue(StringBuf& value) override;
	void getColumnLabel(StringBuf& label) override { label.append(compactSettingLabel(field_)); }

private:
	SettingsContext& context_;
	ls::MenuField field_;
};

class DurationSetting final : public Integer {
public:
	explicit DurationSetting(SettingsContext& context);
	bool isRelevant(ModControllableAudio*, int32_t) const override {
		return context_.relevant(ls::MenuField::Duration);
	}
	void beginSession(MenuItem* = nullptr) override { readCurrentValue(); }
	void readCurrentValue() override { setValue(context_.read(ls::MenuField::Duration)); }
	void writeCurrentValue() override;
	void selectEncoderAction(int32_t offset) override;
	MenuItem* selectButtonPress() override { return NO_NAVIGATION; }
	void getNotificationValue(StringBuf& value) override;
	void getColumnLabel(StringBuf& label) override { label.append(compactSettingLabel(ls::MenuField::Duration)); }

protected:
	int32_t getMaxValue() const override { return context_.maxDurationBars(); }
	RenderingStyle getRenderingStyle() const override { return NUMBER; }

private:
	SettingsContext& context_;
};

class TransposeSetting final : public Decimal {
public:
	explicit TransposeSetting(SettingsContext& context);
	bool isRelevant(ModControllableAudio*, int32_t) const override {
		return context_.relevant(ls::MenuField::Transpose);
	}
	void beginSession(MenuItem* = nullptr) override { readCurrentValue(); }
	void readCurrentValue() override { setValue(context_.read(ls::MenuField::Transpose)); }
	void writeCurrentValue() override;
	void selectEncoderAction(int32_t offset) override;
	void horizontalEncoderAction(int32_t) override {} // No sound-editor decimal cursor in this host.
	MenuItem* selectButtonPress() override { return NO_NAVIGATION; }
	void getColumnLabel(StringBuf& label) override { label.append(compactSettingLabel(ls::MenuField::Transpose)); }

protected:
	int32_t getMinValue() const override { return -2450; }
	int32_t getMaxValue() const override { return 2450; }
	int32_t getNumDecimalPlaces() const override { return 2; }
	int32_t getNumberEditSize() override;

private:
	SettingsContext& context_;
};

class CopySetting final : public MenuItem {
public:
	explicit CopySetting(SettingsContext& context);
	void setTarget(ls::SlotId target) { target_ = target; }
	void getColumnLabel(StringBuf& label) override { label.append(compactSettingLabel(ls::MenuField::CopyToTrack)); }
	void renderInHorizontalMenu(const SlotPosition& slot) override;
	bool isRelevant(ModControllableAudio*, int32_t) const override {
		return context_.relevant(ls::MenuField::CopyToTrack);
	}
	MenuItem* selectButtonPress() override { return NO_NAVIGATION; } // Copy belongs to the host.

private:
	SettingsContext& context_;
	ls::SlotId target_ = 1;
};

class SettingsItems {
public:
	explicit SettingsItems(SettingsContext& context);
	std::span<MenuItem*> all() { return items_; }
	MenuItem* item(ls::MenuField field);
	// Precondition: item is one of all(). No foreign-item sentinel is part of MenuField.
	ls::MenuField field(const MenuItem* item) const;

private:
	std::array<EnumSetting, 9> enums_;
	DurationSetting duration_;
	TransposeSetting transpose_;
	CopySetting copy_;
	std::array<MenuItem*, 12> items_;
};
} // namespace deluge::gui::menu_item::loop_station
