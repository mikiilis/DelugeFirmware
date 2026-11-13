#pragma once

#include "../mocks/audio_clip.h"
#include "CppUTest/TestHarness.h"
#include "probe_support.h"
#include <cstdlib>
#include <memory>

namespace loop_ui_test {
struct Allocator : loop_station::ReservationAllocator {
	void* allocatePinnedLowSpeed(size_t size) override { return std::malloc(size); }
	void deallocate(void* pointer) override { std::free(pointer); }
};
struct Host : loop_station::BindingHost {
	std::vector<AudioClip*> published;
	bool boundary = false;
	bool failAllocation = false;
	unsigned allocationAttempts = 0;
	loop_station::SampleClock time = 100;
	AudioClip* allocateClip() override {
		CHECK_FALSE(boundary);
		++allocationAttempts;
		return failAllocation ? nullptr : new AudioClip;
	}
	loop_station::Status prepareClip(AudioClip* clip, loop_station::SlotId slot) override {
		CHECK_FALSE(boundary);
		clip->name = "LOOP " + std::to_string(slot + 1);
		clip->output = slot + 1;
		return loop_station::Status::Ok;
	}
	loop_station::Status publishClip(AudioClip* clip) override {
		CHECK_TRUE(boundary);
		published.push_back(clip);
		return loop_station::Status::Ok;
	}
	void discardClip(AudioClip* clip) override { delete clip; }
	loop_station::Status attach(AudioClip* clip, loop_station::AudioAdapter* adapter, loop_station::SlotId) override {
		CHECK_TRUE(boundary);
		clip->adapter = adapter;
		return loop_station::Status::Ok;
	}
	bool enterBoundary() override {
		const bool old = boundary;
		boundary = true;
		return old;
	}
	void leaveBoundary(bool old) override { boundary = old; }
	void registerAudio(loop_station::AudioAdapter*, const loop_station::ClockMap*) override { CHECK_TRUE(boundary); }
	loop_station::SampleClock now() const override { return time; }
	~Host() override {
		for (auto* clip : published) {
			CHECK_FALSE(clip->adapter);
			delete clip;
		}
	}
};
struct Fixture {
	Allocator allocator;
	Host host;
	std::unique_ptr<loop_station::SongModel> model = std::make_unique<loop_station::SongModel>(allocator, host);
	Song song;
	ProbeDisplay screen;
	LoopStationView view;
	Fixture() {
		song.model = model.get();
		currentSong = &song;
		display = &screen;
		sdRoutineLock = false;
		Buttons::shiftPressed = false;
		probeRenderRequests = 0;
		probeRenderTargets.clear();
		probeOledRequests = 0;
		probeSampleClock = host.time;
		deluge::gui::menu_item::loop_station::loopStationSettingsMenu = {};
		CHECK_TRUE(openUI(&view));
		auto settings = loop_station::defaultSettings();
		settings.startAction.mode = settings.stopAction.mode = loop_station::ActionMode::Immediate;
		CHECK(loop_station::Status::Ok == model->dispatch(loop_station::Command::configure(settings)));
	}
	~Fixture() {
		model->requestRelease();
		for (unsigned i = 0; i < 10000 && !model->safeToRelease(); ++i) {
			model->serviceRetirement();
			model->serviceArchive(4096);
		}
		CHECK_TRUE(model->safeToRelease());
		model.reset();
		currentSong = nullptr;
		display = nullptr;
		probeCurrentUI = nullptr;
		probeRootUI = nullptr;
		probeRenderTargets.clear();
		sdRoutineLock = false;
		Buttons::shiftPressed = false;
	}
	void onlyBound(loop_station::SlotId slot) {
		for (loop_station::SlotId id = 0; id < 8; ++id)
			CHECK_EQUAL(id == slot, model->clip(id) != nullptr);
		CHECK_EQUAL(1, host.published.size());
	}
	void renderAudio(size_t frames = 8) {
		std::array<loop_station::Frame, loop_station::AudioAdapter::kMaxWindowFrames> input{}, output{};
		CHECK(frames <= input.size());
		input.fill({100, -100});
		size_t count = frames;
		model->adapter().finalizeWindow(host.time, count, model->clock());
		for (loop_station::SlotId slot = 0; slot < 8; ++slot)
			if (model->clip(slot))
				model->adapter().renderDry(slot, std::span(input).first(count), std::span(output).first(count));
		host.time += count;
		probeSampleClock = host.time;
	}
	void recordPhrase() {
		view.padAction(1, 6, 100);
		renderAudio();
		view.padAction(1, 6, 0);
		view.padAction(1, 6, 100);
		CHECK_TRUE(model->engine().state(0).hasPhrase);
		CHECK_TRUE(model->engine().state(0).playing);
	}
	// Simulate the shared button layer only after the real view declines the event.
	template <typename View>
	ActionResult sharedShift(View& target, bool on, bool inCardRoutine = false) {
		const auto result = target.buttonAction(deluge::hid::button::SHIFT, on, inCardRoutine);
		if (result == ActionResult::NOT_DEALT_WITH)
			Buttons::shiftPressed = on;
		return result;
	}
};
} // namespace loop_ui_test
