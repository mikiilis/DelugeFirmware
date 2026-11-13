#include "ui/fixture.h"

using namespace loop_station;
using loop_ui_test::Fixture;
namespace colours = deluge::gui::colours;

namespace {
// Physical coordinates, independently transcribed from the approved layout.
constexpr int32_t topRing[12][2] = {{0, 7}, {1, 7}, {2, 7}, {3, 7}, {3, 6}, {3, 5},
                                    {3, 4}, {2, 4}, {1, 4}, {0, 4}, {0, 5}, {0, 6}};
constexpr int32_t bottomRing[12][2] = {{0, 3}, {1, 3}, {2, 3}, {3, 3}, {3, 2}, {3, 1},
                                       {3, 0}, {2, 0}, {1, 0}, {0, 0}, {0, 1}, {0, 2}};
constexpr RGB sentinel{17, 29, 43};
constexpr uint8_t sentinelMask = 99;

struct Pads {
	RGB image[kDisplayHeight][kDisplayWidth + kSideBarWidth];
	uint8_t mask[kDisplayHeight][kDisplayWidth + kSideBarWidth];
	Pads() {
		for (int32_t y = 0; y < kDisplayHeight; ++y) {
			for (int32_t x = 0; x < kDisplayWidth + kSideBarWidth; ++x) {
				image[y][x] = sentinel;
				mask[y][x] = sentinelMask;
			}
		}
	}
	void expect(int32_t x, int32_t y, RGB colour, uint8_t occupancy = 1) const {
		CHECK_EQUAL(colour.r, image[y][x].r);
		CHECK_EQUAL(colour.g, image[y][x].g);
		CHECK_EQUAL(colour.b, image[y][x].b);
		CHECK_EQUAL(occupancy, mask[y][x]);
	}
};

// Keep renderer setup independent of the mapping under test. Capture actual audio,
// not a fabricated engine state, and explicitly choose a stable test colour.
void record120Frames(Fixture& f, SlotId slot) {
	auto settings = f.model->engine().settings();
	settings.tracks[slot].colour = Colour::Yellow;
	CHECK(Status::Ok == f.model->dispatch(Command::configure(settings)));
	CHECK(Status::Ok == f.model->ensureBound(slot));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, slot}));
	f.renderAudio(120);
	CHECK(Status::Ok == f.model->dispatch({CommandType::Release, slot}));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Press, slot}));
	CHECK(Status::Ok == f.model->dispatch({CommandType::Release, slot}));
	CHECK_TRUE(f.model->engine().state(slot).playing);
	CHECK_EQUAL(120, f.model->engine().state(slot).phraseFrames);
	CHECK_EQUAL(0, f.model->adapter().phase(slot));
}

void expectRing(const Pads& pads, const int32_t (&ring)[12][2], uint8_t brightPads, RGB colour) {
	for (uint8_t i = 0; i < 12; ++i)
		pads.expect(ring[i][0], ring[i][1], i < brightPads ? colour : colour.dim(3));
}
} // namespace

TEST_GROUP(LoopPadRender){};

TEST(LoopPadRender, TopLeftSlotAndInputAgree) {
	Fixture f;
	record120Frames(f, 0);
	Pads pads;
	CHECK_TRUE(f.view.renderMainPads(0xff, pads.image, pads.mask, false));
	pads.expect(0, 7, colours::yellow);
	pads.expect(1, 6, colours::green);
	pads.expect(1, 2, RGB::monochrome(40));
	f.view.padAction(0, 7, 100);
	CHECK_EQUAL(0, f.model->engine().selected());
	f.onlyBound(0);
	f.view.padAction(1, 6, 100);
	CHECK_TRUE(f.model->engine().state(0).capture);
	CHECK_FALSE(f.model->engine().state(4).capture);
	CHECK_TRUE(f.view.renderMainPads(0xff, pads.image, pads.mask, false));
	pads.expect(1, 6, colours::orange);
}

TEST(LoopPadRender, PlayingRingVisitsPhysicalClockwisePads) {
	for (SlotId slot : {0, 4}) {
		Fixture f;
		record120Frames(f, slot);
		for (uint8_t step = 0; step <= 12; ++step) {
			if (step)
				f.renderAudio(10);
			CHECK_EQUAL((step * 10) % 120, f.model->adapter().phase(slot));
			Pads pads;
			CHECK_TRUE(f.view.renderMainPads(0xff, pads.image, pads.mask, false));
			const uint8_t bright = step == 12 ? 1 : step + 1;
			expectRing(pads, slot == 0 ? topRing : bottomRing, bright, colours::yellow);
			pads.expect(1, slot == 0 ? 6 : 2, colours::green);
			pads.expect(1, slot == 0 ? 2 : 6, RGB::monochrome(40));
		}
	}
}

TEST(LoopPadRender, ClearSweepUsesSamePhysicalOrder) {
	for (SlotId slot : {0, 4}) {
		Fixture f;
		record120Frames(f, slot);
		CHECK(Status::Ok == f.model->dispatch({CommandType::StopAll}));
		CHECK(Status::Ok == f.model->dispatch({CommandType::Press, slot}));
		for (uint8_t step = 1; step <= 12; ++step) {
			probeSampleClock = f.host.time + step * 7350; // 88200 / 12 samples per pad.
			Pads pads;
			CHECK_TRUE(f.view.renderMainPads(0xff, pads.image, pads.mask, false));
			expectRing(pads, slot == 0 ? topRing : bottomRing, step, colours::red);
			pads.expect(1, slot == 0 ? 6 : 2, colours::red);
			pads.expect(1, slot == 0 ? 2 : 6, RGB::monochrome(40));
			CHECK_FALSE(f.model->engine().state(slot).playing);
		}
	}
}

TEST(LoopPadRender, SidebarColoursAndActionsShareTopRow) {
	Fixture f;
	record120Frames(f, 0);
	Pads pads;
	CHECK_TRUE(f.view.renderSidebar(0xff, pads.image, pads.mask));
	pads.expect(16, 7, colours::green.dim(3));
	pads.expect(17, 7, colours::red);
	for (int32_t y = 0; y < 7; ++y) {
		pads.expect(16, y, colours::black);
		pads.expect(17, y, colours::black);
	}
	f.view.padAction(17, 7, 100);
	CHECK_FALSE(f.model->engine().state(0).playing);
	CHECK_TRUE(f.view.renderSidebar(0xff, pads.image, pads.mask));
	pads.expect(16, 7, colours::green);
	pads.expect(17, 7, colours::red.dim(3));
	f.view.padAction(16, 7, 100);
	CHECK_TRUE(f.model->engine().state(0).playing);
	CHECK_TRUE(f.view.renderSidebar(0xff, pads.image, pads.mask));
	pads.expect(16, 7, colours::green.dim(3));
	pads.expect(17, 7, colours::red);
	f.onlyBound(0);
}

TEST(LoopPadRender, PartialRowsLeaveOtherPixelsUntouched) {
	Fixture f;
	record120Frames(f, 0);
	Pads main;
	CHECK_TRUE(f.view.renderMainPads(1u << 7, main.image, main.mask, false));
	main.expect(0, 7, colours::yellow);
	for (int32_t y = 0; y < kDisplayHeight; ++y) {
		for (int32_t x = 0; x < kDisplayWidth + kSideBarWidth; ++x) {
			if (y != 7 || x >= kDisplayWidth)
				main.expect(x, y, sentinel, sentinelMask);
			else
				CHECK_EQUAL(1, main.mask[y][x]);
		}
	}
	Pads sidebar;
	CHECK_TRUE(f.view.renderSidebar(1u << 7, sidebar.image, sidebar.mask));
	sidebar.expect(16, 7, colours::green.dim(3));
	sidebar.expect(17, 7, colours::red);
	for (int32_t y = 0; y < kDisplayHeight; ++y)
		for (int32_t x = 0; x < kDisplayWidth + kSideBarWidth; ++x)
			if (y != 7 || x < kDisplayWidth)
				sidebar.expect(x, y, sentinel, sentinelMask);
}
