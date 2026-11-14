#include "deluge/loop_station/persistence.h"
#include "fixture.h"
#include "settings_items_declarations.h"
#include "shared_probe_support.h"
#include <climits>

using namespace loop_station;
using namespace deluge::gui::menu_item;
namespace native = deluge::gui::menu_item::loop_station;
using loop_ui_test::Fixture;

// clang-format off
TEST_GROUP(LoopSettingsItems) {
	void teardown() override {
		Buttons::shiftPressed = Buttons::selectPressed = Buttons::selectButtonPressUsedUp = false;
	}
};
// clang-format on

TEST(LoopSettingsItems, NativeFieldsMatchExistingSettings) {
	Fixture f;
	native::SettingsContext context;
	native::SettingsItems items(context);
	CHECK_EQUAL(12, items.all().size());
	for (auto* item : items.all()) {
		POINTERS_EQUAL(item, items.item(items.field(item)));
		CHECK_EQUAL(1, item->getOccupiedSlots());
	}
	const MenuField fields[] = {MenuField::RecordAction, MenuField::StartMode,    MenuField::StartInterval,
	                            MenuField::StopMode,     MenuField::StopInterval, MenuField::Source,
	                            MenuField::Monitoring,   MenuField::Playback,     MenuField::Colour};
	const int counts[] = {2, 2, 12, 2, 12, 4, 2, 2, 11};
	for (unsigned i = 0; i < std::size(fields); ++i) {
		context.scope = i < 5 ? MenuScope::Global : MenuScope::Track;
		context.slot = 3;
		auto* item = static_cast<native::EnumSetting*>(items.item(fields[i]));
		CHECK_EQUAL(counts[i], item->getOptions().size());
		for (int value = 0; value < counts[i]; ++value) {
			item->setValue(value);
			item->writeCurrentValue();
			CHECK_EQUAL(value, context.read(fields[i]));
			const auto& settings = f.model->engine().settings();
			const auto& track = settings.tracks[3];
			switch (fields[i]) {
			case MenuField::RecordAction:
				CHECK_EQUAL(value, int(settings.recordAction));
				break;
			case MenuField::StartMode:
				CHECK_EQUAL(value, int(settings.startAction.mode));
				break;
			case MenuField::StartInterval:
				CHECK(intervalAtMenuIndex(value) == settings.startAction.interval);
				break;
			case MenuField::StopMode:
				CHECK_EQUAL(value, int(settings.stopAction.mode));
				break;
			case MenuField::StopInterval:
				CHECK(intervalAtMenuIndex(value) == settings.stopAction.interval);
				break;
			case MenuField::Source:
				CHECK_EQUAL(value, int(track.source));
				break;
			case MenuField::Monitoring:
				CHECK_EQUAL(value, int(track.monitoring));
				break;
			case MenuField::Playback:
				CHECK_EQUAL(value, int(track.playback));
				break;
			case MenuField::Colour:
				CHECK(trackColourAt(value) == track.colour);
				break;
			default:
				FAIL("Unexpected enumeration");
			}
			item->setValue(-1);
			item->readCurrentValue();
			CHECK_EQUAL(value, item->getValue());
		}
		item->selectEncoderAction(INT32_MAX);
		CHECK_EQUAL((counts[i] - 1LL + INT32_MAX) % counts[i], context.read(fields[i]));
		item->selectEncoderAction(INT32_MIN);
		CHECK(context.read(fields[i]) >= 0);
		CHECK(context.read(fields[i]) < counts[i]);
	}
	const auto& s = f.model->engine().settings();
	CHECK(s.startAction.interval == intervalAtMenuIndex(context.read(MenuField::StartInterval)));
	CHECK(s.stopAction.interval == intervalAtMenuIndex(context.read(MenuField::StopInterval)));
	CHECK(s.tracks[3].colour == trackColourAt(context.read(MenuField::Colour)));
	CHECK_EQUAL(0, f.host.allocationAttempts); // Item binding does not eagerly allocate clips.
	auto* duration = static_cast<native::DurationSetting*>(items.item(MenuField::Duration));
	duration->beginSession(nullptr);
	duration->selectEncoderAction(INT32_MAX);
	CHECK_EQUAL(f.model->maxDurationBarsForTrack(3, f.model->clock().samplesForBars(1)), context.maxDurationBars());
	CHECK_EQUAL(context.maxDurationBars(), context.read(MenuField::Duration));
	duration->selectEncoderAction(INT32_MIN);
	CHECK_EQUAL(0, context.read(MenuField::Duration));
	StringBuf notification;
	duration->getNotificationValue(notification);
	STRCMP_EQUAL("Manual", notification.c_str());
	const auto oldMaximum = context.maxDurationBars();
	f.model->updateClock(f.host.time, 60u << 16, false, 0);
	CHECK(context.maxDurationBars() < oldMaximum);
	duration->selectEncoderAction(INT32_MAX);
	CHECK_EQUAL(context.maxDurationBars(), context.read(MenuField::Duration));
}

TEST(LoopSettingsItems, VisibilityTracksBothModeFlags) {
	Fixture f;
	native::SettingsContext context;
	native::SettingsItems items(context);
	for (int start = 0; start < 2; ++start) {
		for (int stop = 0; stop < 2; ++stop) {
			CHECK(Status::Ok == context.write(MenuField::StartMode, start));
			CHECK(Status::Ok == context.write(MenuField::StopMode, stop));
			std::vector<MenuField> expected{MenuField::RecordAction, MenuField::StartMode};
			if (start)
				expected.push_back(MenuField::StartInterval);
			expected.push_back(MenuField::StopMode);
			if (stop)
				expected.push_back(MenuField::StopInterval);
			const auto visible = visibleMenuFields(context.scope, f.model->engine().settings());
			CHECK_EQUAL(expected.size(), visible.count);
			for (unsigned i = 0; i < visible.count; ++i)
				CHECK(expected[i] == visible.rows[i]);
			for (auto* item : items.all())
				CHECK_EQUAL(visible.contains(items.field(item)), context.relevant(items.field(item)));
		}
	}
	context.scope = MenuScope::Track;
	for (auto* item : items.all())
		CHECK_EQUAL(int(items.field(item)) >= int(MenuField::Source), context.relevant(items.field(item)));
}

TEST(LoopSettingsItems, RejectedWriteRestoresAuthoritativeValue) {
	Fixture f;
	native::SettingsContext context{MenuScope::Track, 2};
	native::SettingsItems items(context);
	auto* source = static_cast<native::EnumSetting*>(items.item(MenuField::Source));
	source->readCurrentValue();
	const auto old = source->getValue();
	StringBuf before, after;
	source->getNotificationValue(before);
	source->setValue(99); // Real invalid setting, not a mocked dispatch.
	source->writeCurrentValue();
	CHECK_EQUAL(old, source->getValue());
	CHECK_EQUAL(old, int(f.model->engine().settings().tracks[2].source));
	source->getNotificationValue(after);
	STRCMP_EQUAL(before.c_str(), after.c_str());
	CHECK(Status::Invalid == context.write(MenuField::Source, 99));
	CHECK(Status::Invalid == context.write(MenuField::Transpose, 2451));
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		CHECK_EQUAL(int(defaultSettings().tracks[slot].colour), int(f.model->engine().settings().tracks[slot].colour));
	CHECK_EQUAL(0, f.host.allocationAttempts);
	// A real SongModel rejection also restores the authoritative value and notification.
	f.model->requestRelease();
	CHECK(Status::Unavailable == context.write(MenuField::Source, 1));
	source->selectEncoderAction(1);
	CHECK_EQUAL(old, source->getValue());
	StringBuf rejected;
	source->getNotificationValue(rejected);
	STRCMP_EQUAL(before.c_str(), rejected.c_str());
}

TEST(LoopSettingsItems, NegativeTransposeRetainsTwoDecimalsInSharedRenderer) {
	Fixture f;
	native::SettingsContext context{MenuScope::Track, 0};
	native::SettingsItems items(context);
	auto* transpose = static_cast<native::TransposeSetting*>(items.item(MenuField::Transpose));
	CHECK(Status::Ok == context.write(MenuField::Transpose, -2450));
	transpose->readCurrentValue();
	transpose->renderInHorizontalMenu({0, 14, 31, 25});
	STRCMP_EQUAL("-24.50", deluge::hid::display::OLED::main.lines.back().c_str());
}

TEST(LoopSettingsItems, AcceptedSelectAndInstrumentReselectionHaveNoCommandOrEditorSideEffects) {
	struct Host : HorizontalMenu {
		using HorizontalMenu::HorizontalMenu;
		void beginHostSession(MenuItem* from) override { focusChild(from); }
		void endHostSession() override {}
		bool isChildRelevant(MenuItem*) const override { return true; }
		MenuPermission initializeItem(MenuItem* item) override {
			item->beginSession(nullptr);
			return MenuPermission::YES;
		}
		bool handleItemPermission(MenuItem*, MenuPermission p) override { return p == MenuPermission::YES; }
		void handleItemAction(MenuItem* item) override { POINTERS_EQUAL(NO_NAVIGATION, item->selectButtonPress()); }
		bool trySwitchHorizontalMenu(int32_t) override { return false; }
		double calcNextKnobSpeed(int8_t) override { return 1; }
		void updatePadLights() override {}
	};
	Fixture f;
	native::SettingsContext context;
	native::SettingsItems items(context);
	soundEditor = {};
	soundEditor.numberEditPos = 7;
	soundEditor.numberEditSize = 1000;
	soundEditor.numberScrollAmount = 3;
	for (auto* item : items.all()) {
		context.scope = items.field(item) <= MenuField::StopInterval ? MenuScope::Global : MenuScope::Track;
		Host menu({}, {item});
		menu.beginSession(nullptr);
		const auto before = context.read(items.field(item));
		for (int n = 0; n < 3; ++n) {
			POINTERS_EQUAL(NO_NAVIGATION, menu.selectButtonPress());
			probeMenuTime += 1;
			menu.buttonAction(deluge::hid::button::SYNTH, true, false);
			CHECK_EQUAL(before, context.read(items.field(item)));
		}
		CHECK_EQUAL(0, soundEditor.navigationDepth);
		CHECK_EQUAL(0, soundEditor.entries);
		CHECK_EQUAL(0, soundEditor.exits);
		CHECK_EQUAL(7, soundEditor.numberEditPos);
		CHECK_EQUAL(1000, soundEditor.numberEditSize);
		CHECK_EQUAL(3, soundEditor.numberScrollAmount);
		menu.endSession();
	}
	CHECK_EQUAL(0, f.host.allocationAttempts);
}

TEST(LoopSettingsItems, TransposeCoarseFineAndClamp) {
	CHECK_EQUAL(223, stepTransposeCents(123, 1, false));
	CHECK_EQUAL(124, stepTransposeCents(123, 1, true));
	CHECK_EQUAL(-123, stepTransposeCents(-23, -1, false));
	CHECK_EQUAL(2450, stepTransposeCents(2449, 1, false));
	CHECK_EQUAL(-2450, stepTransposeCents(-2449, -1, true));
	CHECK_EQUAL(2450, stepTransposeCents(0, INT32_MAX, false));
	CHECK_EQUAL(-2450, stepTransposeCents(0, INT32_MIN, true));
	Fixture f;
	native::SettingsContext context{MenuScope::Track, 4};
	native::SettingsItems items(context);
	auto* transpose = static_cast<native::TransposeSetting*>(items.item(MenuField::Transpose));
	CHECK(Status::Ok == context.write(MenuField::Transpose, 123));
	HorizontalMenu menu({}, {transpose});
	transpose->parent = &menu;
	soundEditor.numberEditPos = 7;
	soundEditor.numberEditSize = 10000;
	soundEditor.numberScrollAmount = 3;
	transpose->beginSession(nullptr);
	transpose->selectEncoderAction(1);
	CHECK_EQUAL(223, context.read(MenuField::Transpose));
	Buttons::shiftPressed = true;
	transpose->selectEncoderAction(1);
	CHECK_EQUAL(224, context.read(MenuField::Transpose));
	Buttons::shiftPressed = false;
	Buttons::selectPressed = true;
	transpose->selectEncoderAction(-1);
	CHECK_EQUAL(223, context.read(MenuField::Transpose));
	CHECK_TRUE(Buttons::selectButtonPressUsedUp);
	transpose->horizontalEncoderAction(1);
	CHECK_EQUAL(7, soundEditor.numberEditPos);
	CHECK_EQUAL(10000, soundEditor.numberEditSize);
	CHECK_EQUAL(3, soundEditor.numberScrollAmount);
	for (auto* item : items.all())
		POINTERS_EQUAL(NO_NAVIGATION, item->selectButtonPress());
	CHECK_EQUAL(223, context.read(MenuField::Transpose));
	StringBuf value;
	transpose->getNotificationValue(value);
	STRCMP_EQUAL("2.23", value.c_str());
}

TEST(LoopSettingsItems, EditedTrackSettingsRoundTrip) {
	Fixture f;
	native::SettingsContext context{MenuScope::Track, 0};
	native::SettingsItems items(context);
	Manifest manifest{};
	manifest.generation = 42;
	manifest.phraseCount = kTrackCount;
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		context.slot = slot;
		for (const auto field : {MenuField::Source, MenuField::Monitoring, MenuField::Playback, MenuField::Colour}) {
			auto* item = static_cast<native::EnumSetting*>(items.item(field));
			item->setValue(field == MenuField::Colour ? 10 : 1);
			item->writeCurrentValue();
		}
		auto* transpose = static_cast<native::TransposeSetting*>(items.item(MenuField::Transpose));
		transpose->beginSession(nullptr);
		transpose->selectEncoderAction(slot + 1);
		Buttons::shiftPressed = true;
		transpose->selectEncoderAction(23);
		Buttons::shiftPressed = false;
		auto* duration = static_cast<native::DurationSetting*>(items.item(MenuField::Duration));
		duration->selectEncoderAction(1);
		auto& phrase = manifest.phrases[slot];
		phrase.slot = slot;
		phrase.identity = 1000 + slot;
		phrase.frames = 44100 + slot;
		std::snprintf(phrase.path.data(), phrase.path.size(), "L%1X000000.WAV", unsigned(slot + 1));
	}
	SettingsDocument encoded{};
	CHECK(Status::Ok == Persistence::encode(f.model->engine().settings(), manifest, encoded));
	std::swap(encoded.tracks[0], encoded.tracks[5]);
	Settings restored{};
	Manifest audio{};
	CHECK(Status::Ok == Persistence::decode(encoded, restored, audio));
	CHECK_EQUAL(42, audio.generation);
	CHECK_EQUAL(kTrackCount, audio.phraseCount);
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		CHECK_EQUAL((slot + 1) * 100 + 23, restored.tracks[slot].transposeCents);
		CHECK(restored.tracks[slot].colour == Colour::White);
		CHECK(restored.tracks[slot].source == Source::Right);
		CHECK(restored.tracks[slot].monitoring == Monitoring::On);
		CHECK_EQUAL(1, restored.tracks[slot].recordDurationBars);
		CHECK(restored.tracks[slot].playback == ::loop_station::PlaybackMode::OneShot);
		CHECK_EQUAL(slot, audio.phrases[slot].slot);
		CHECK_EQUAL(1000 + slot, audio.phrases[slot].identity);
		CHECK_EQUAL(44100 + slot, audio.phrases[slot].frames);
		STRCMP_EQUAL(manifest.phrases[slot].path.data(), audio.phrases[slot].path.data());
	}
}

TEST(LoopSettingsItems, LiveEditsDuringCapturePreserveLatestUnrelatedSettings) {
	Fixture f;
	f.view.padAction(1, 6, 100);
	CHECK_TRUE(f.model->engine().state(0).capture);
	native::SettingsContext context{MenuScope::Track, 0};
	native::SettingsItems items(context);
	auto* transpose = static_cast<native::TransposeSetting*>(items.item(MenuField::Transpose));
	transpose->beginSession(nullptr);
	auto settings = f.model->engine().settings();
	settings.tracks[7].colour = Colour::White;
	settings.tracks[7].transposeCents = -123;
	settings.recordAction = RecordAction::REC_DUB_PLAY;
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	transpose->selectEncoderAction(1);
	CHECK_EQUAL(100, f.model->engine().settings().tracks[0].transposeCents);
	CHECK_EQUAL(-123, f.model->engine().settings().tracks[7].transposeCents);
	CHECK(f.model->engine().settings().tracks[7].colour == Colour::White);
	CHECK(f.model->engine().settings().recordAction == RecordAction::REC_DUB_PLAY);
	CHECK_TRUE(f.model->engine().state(0).capture);
	context.scope = MenuScope::Global;
	auto* action = static_cast<native::EnumSetting*>(items.item(MenuField::RecordAction));
	action->selectEncoderAction(1);
	CHECK(f.model->engine().settings().recordAction == RecordAction::REC_PLAY_DUB);
	CHECK_TRUE(f.model->engine().state(0).capture);
	CHECK_EQUAL(100, f.model->engine().settings().tracks[0].transposeCents);
}
