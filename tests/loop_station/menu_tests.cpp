#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/engine.h"
#include "deluge/loop_station/menu_model.h"
#include <array>
#include <vector>

using namespace loop_station;

TEST_GROUP(LoopMenus){};

TEST(LoopMenus, SyncedOnlyIntervalsAppearInGlobalMenu) {
	const auto settings = defaultSettings();
	const auto synced = visibleMenuFields(MenuScope::Global, settings);
	CHECK_TRUE(synced.contains(MenuField::StartInterval));
	CHECK_TRUE(synced.contains(MenuField::StopInterval));
	CHECK_EQUAL(static_cast<int>(MenuField::RecordAction), static_cast<int>(synced.rows[0]));
	CHECK_EQUAL(static_cast<int>(MenuField::StartMode), static_cast<int>(synced.rows[1]));
	CHECK_EQUAL(static_cast<int>(MenuField::StartInterval), static_cast<int>(synced.rows[2]));
	CHECK_EQUAL(static_cast<int>(MenuField::StopMode), static_cast<int>(synced.rows[3]));
	CHECK_EQUAL(static_cast<int>(MenuField::StopInterval), static_cast<int>(synced.rows[4]));
	auto immediate = settings;
	immediate.startAction.mode = ActionMode::Immediate;
	immediate.stopAction.mode = ActionMode::Immediate;
	const auto rows = visibleMenuFields(MenuScope::Global, immediate);
	CHECK_FALSE(rows.contains(MenuField::StartInterval));
	CHECK_FALSE(rows.contains(MenuField::StopInterval));
	CHECK_TRUE(rows.contains(MenuField::StartMode));
	CHECK_TRUE(rows.contains(MenuField::StopMode));
}

TEST(LoopMenus, TrackFieldsHaveSpecifiedOrderWithoutLegacyTimingRows) {
	const auto fields = visibleMenuFields(MenuScope::Track, defaultSettings());
	const MenuField expected[] = {MenuField::Source,     MenuField::Monitoring, MenuField::Duration,
	                              MenuField::Playback,   MenuField::Transpose,  MenuField::Colour,
	                              MenuField::CopyToTrack};
	CHECK_EQUAL(sizeof(expected) / sizeof(expected[0]), fields.count);
	for (unsigned i = 0; i < fields.count; ++i)
		CHECK_EQUAL(static_cast<int>(expected[i]), static_cast<int>(fields.rows[i]));
	CHECK_FALSE(fields.contains(MenuField::StartInterval));
	CHECK_FALSE(fields.contains(MenuField::StopInterval));
}

TEST(LoopMenus, BackAndUnshiftedSessionCloseWhileShiftedSessionStaysInMenu) {
	CHECK_TRUE(shouldCloseSettingsMenu(true, false, false));
	CHECK_TRUE(shouldCloseSettingsMenu(false, true, false));
	CHECK_FALSE(shouldCloseSettingsMenu(false, true, true));
	CHECK_FALSE(shouldCloseSettingsMenu(false, false, false));
}

TEST(LoopMenus, DurationAndTransposeRespectSupportedBounds) {
	CHECK_EQUAL(11, maxDurationBars(MemoryProfile::Preferred, 44100));
	CHECK_EQUAL(10, maxDurationBars(MemoryProfile::TenSecond, 44100));
	CHECK_EQUAL(0, maxDurationBars(MemoryProfile::Preferred, 0));
	CHECK_EQUAL(1024, maxDurationBarsForPages(8192, 512));
	CHECK_EQUAL(1023, maxDurationBarsForPages(8192 - 8, 512));
	CHECK_EQUAL(0, maxDurationBarsForPages(8192, 0));
	CHECK_EQUAL(2450, clampTransposeCents(3000));
	CHECK_EQUAL(-2450, clampTransposeCents(-3000));
	CHECK_EQUAL(100, clampTransposeCents(100));
	CHECK_EQUAL(1, stepTransposeCents(0, 1));
	CHECK_EQUAL(100, stepTransposeCents(99, 1));
	CHECK_EQUAL(-1, stepTransposeCents(0, -1));
	CHECK_EQUAL(2450, stepTransposeCents(2449, 1));
	CHECK_EQUAL(-2450, stepTransposeCents(-2449, -1));
}

TEST(LoopMenus, SettingsConfigurationRemainsUsableDuringCapture) {
	std::vector<std::byte> backing(400 * kPageBytes), scratch(GestureRollback::requiredBytes());
	PagePool pool;
	GestureRollback rollback{pool};
	ResidentAudio audio{pool, rollback};
	Engine engine{audio, rollback};
	ClockMap clock;
	CHECK(Status::Ok == pool.initialize(backing));
	CHECK(Status::Ok == rollback.initialize(scratch));
	auto settings = defaultSettings();
	settings.startAction.mode = ActionMode::Immediate;
	settings.stopAction.mode = ActionMode::Immediate;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 0, clock));
	CHECK(Status::Ok == engine.dispatch({CommandType::Press, 0}, 0, clock));
	CHECK(engine.state(0).capture);
	settings = engine.settings();
	settings.tracks[0].transposeCents = 700;
	settings.tracks[0].colour = Colour::Cyan;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 1, clock));
	CHECK(engine.state(0).capture);
	CHECK_EQUAL(700, engine.settings().tracks[0].transposeCents);
	CHECK(engine.settings().tracks[0].colour == Colour::Cyan);
}

TEST(LoopMenus, ColourMenuCoversEverySupportedTrackColour) {
	const Colour expected[] = {Colour::Gold,    Colour::Yellow, Colour::Lime, Colour::Turquoise,
	                           Colour::Cyan,    Colour::Sky,    Colour::Blue, Colour::Purple,
	                           Colour::Magenta, Colour::Pink,   Colour::White};
	CHECK_EQUAL(sizeof(expected) / sizeof(expected[0]), kTrackColourCount);
	for (unsigned i = 0; i < kTrackColourCount; ++i)
		CHECK_EQUAL(static_cast<int>(expected[i]), static_cast<int>(trackColourAt(i)));
}
