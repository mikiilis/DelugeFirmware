#include "CppUTest/TestHarness.h"
#include "deluge/dsp/stereo_sample.h"
#include "deluge/loop_station/archive.h"
#include "deluge/loop_station/audio_adapter.h"
#include <array>
#include <functional>
#include <utility>
#include <vector>
using namespace loop_station;

TEST_GROUP(LoopAudioAdapter) {
	std::vector<std::byte> storage, scratch;
	PagePool pool;
	GestureRollback rollback{pool};
	ResidentAudio audio{pool, rollback};
	Engine engine{audio, rollback};
	Transpose transpose;
	AudioAdapter adapter{engine, audio, pool, transpose};
	ClockMap clock;
	void setup() {
		storage.resize(8 * kPageBytes);
		scratch.resize(GestureRollback::requiredBytes());
		CHECK(Status::Ok == pool.initialize(storage));
		CHECK(Status::Ok == rollback.initialize(scratch));
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			CHECK(Status::Ok == transpose.prepare(slot));
		auto settings = defaultSettings();
		settings.startAction.mode = settings.stopAction.mode = ActionMode::Immediate;
		configure(settings);
	}
	void configure(Settings settings, SampleClock now = 0) {
		CHECK(Status::Ok == engine.dispatch(Command::configure(settings), now, clock));
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			transpose.setCents(slot, settings.tracks[slot].transposeCents);
	}
	void command(CommandType type, SampleClock now, SlotId slot = 0) {
		const auto status = engine.dispatch({type, slot}, now, clock);
		CHECK_TEXT(Status::Ok == status,
		           StringFromFormat("command=%u slot=%u now=%llu status=%u", unsigned(type), unsigned(slot),
		                            static_cast<unsigned long long>(now), unsigned(status))
		               .asCharString());
	}
	void window(SampleClock now, size_t frames = 1) {
		adapter.finalizeWindow(now, frames, clock);
	}
	void record(SlotId slot = 0, Frame value = {100, 100}, SampleClock now = 0) {
		command(CommandType::Press, now, slot);
		window(now, 4);
		Frame in[4], out[4];
		std::fill_n(in, 4, value);
		adapter.renderDry(slot, in, out);
		command(CommandType::Press, now + 4, slot);
	}
};

TEST(LoopAudioAdapter, eightTracksKeepUnequalShortAndLongLoopPhasesDuringSharedOverdubs) {
	std::vector<std::byte> mixedStorage(32 * kPageBytes), mixedScratch(GestureRollback::requiredBytes());
	PagePool mixedPool;
	GestureRollback mixedRollback{mixedPool};
	CHECK(Status::Ok == mixedPool.initialize(mixedStorage));
	CHECK(Status::Ok == mixedRollback.initialize(mixedScratch));
	ResidentAudio mixedAudio{mixedPool, mixedRollback};
	Engine mixedEngine{mixedAudio, mixedRollback};
	Transpose mixedTranspose;
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		CHECK(Status::Ok == mixedTranspose.prepare(slot));
	AudioAdapter mixedAdapter{mixedEngine, mixedAudio, mixedPool, mixedTranspose};
	ClockMap mixedClock;
	auto settings = defaultSettings();
	settings.startAction.mode = settings.stopAction.mode = ActionMode::Immediate;
	CHECK(Status::Ok == mixedEngine.dispatch(Command::configure(settings), 0, mixedClock));
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		CHECK(Status::Ok == mixedEngine.dispatch({CommandType::Press, slot}, 0, mixedClock));
	auto renderAll = [&](SampleClock now, size_t frames) {
		std::array<Frame, 128> input{}, output{};
		input.fill({1, 1});
		mixedAdapter.finalizeWindow(now, frames, mixedClock);
		for (SlotId slot = 0; slot < kTrackCount; ++slot)
			mixedAdapter.renderDry(slot, std::span(input).first(frames), std::span(output).first(frames));
	};
	for (SampleClock now = 0; now < 256; now += 128)
		renderAll(now, 128);
	for (SlotId slot = 4; slot < 8; ++slot)
		CHECK(Status::Ok == mixedEngine.dispatch({CommandType::Press, slot}, 256, mixedClock));
	for (SampleClock now = 256; now < 513; now += 128)
		renderAll(now, std::min<SampleClock>(128, 513 - now));
	for (SlotId slot = 0; slot < 4; ++slot)
		CHECK(Status::Ok == mixedEngine.dispatch({CommandType::Press, slot}, 513, mixedClock));
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		CHECK(mixedEngine.state(slot).playing);
		CHECK_FALSE(mixedEngine.state(slot).capture);
		CHECK_EQUAL(slot < 4 ? 513 : 256, mixedPool.length(mixedPool.sealedHandle(slot)));
		CHECK_EQUAL(slot < 4 ? 0 : 1, mixedAdapter.phase(slot));
		CHECK(Status::Ok == mixedEngine.dispatch({CommandType::Press, slot}, 513, mixedClock));
	}
	const SampleClock finishTime = 513 + GestureRollback::kProvisionalSamples + 1024;
	for (SampleClock now = 513; now < finishTime; now += 128)
		renderAll(now, std::min<SampleClock>(128, finishTime - now));
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		CHECK_EQUAL(slot < 4 ? 43 : 76, mixedAdapter.phase(slot));
	for (SlotId slot = 0; slot < kTrackCount; ++slot) {
		CHECK(Status::Ok == mixedEngine.dispatch({CommandType::Press, slot}, finishTime, mixedClock));
		CHECK_FALSE(mixedEngine.state(slot).capture);
		CHECK_TEXT(mixedEngine.state(slot).playing,
		           StringFromFormat("slot=%u error=%u", slot, unsigned(mixedEngine.state(slot).error)).asCharString());
		CHECK_EQUAL(slot < 4 ? 513 : 256, mixedPool.length(mixedPool.sealedHandle(slot)));
	}
}

TEST(LoopAudioAdapter, fractionalDeadlineUsesCeilingAndDueNowMakesProgressWithoutTransport) {
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	configure(settings);
	clock.setTempo(0, 137);
	command(CommandType::Press, 0);
	size_t frames = 20000;
	adapter.finalizeWindow(0, frames, clock);
	CHECK_EQUAL(19314, frames); // 2646000/137 = 19313.868...
	CHECK_FALSE(engine.state(0).capture);
	frames = 128;
	adapter.finalizeWindow(19314, frames, clock);
	CHECK_EQUAL(128, frames);
	CHECK(engine.state(0).capture);
	frames = 128;
	adapter.finalizeWindow(19314, frames, clock);
	CHECK_EQUAL(128, frames);
}

TEST(LoopAudioAdapter, captureVisitHearsBackingPlusMonitorOnceAndStoresDrySum) {
	for (SlotId slot = 0; slot < 2; ++slot) {
		const auto monitor = slot == 0 ? Monitoring::On : Monitoring::Off;
		auto settings = engine.settings();
		settings.tracks[slot].monitoring = monitor;
		const SampleClock base = slot * 50000;
		configure(settings, base);
		record(slot, {100, 100}, base);
		command(CommandType::Press, base + 20000, slot);
		window(base + 20000);
		Frame in[] = {{10, 30}}, out[1];
		adapter.renderDry(slot, in, out);
		CHECK_EQUAL(monitor == Monitoring::On ? 110 : 100, out[0].l);
		CHECK_EQUAL(out[0].l, out[0].r); // default LEFT
		command(CommandType::Press, base + 40000, slot);
		window(base + 40000, 4);
		Frame silence[4]{}, backing[4];
		adapter.renderDry(slot, silence, backing);
		CHECK_EQUAL(110, backing[3].l);
		CHECK_EQUAL(110, backing[3].r);
	}
}

TEST(LoopAudioAdapter, monitoringOffPlaybackIsTransposedAfterDryCapture) {
	std::vector<std::byte> backing(32 * kPageBytes), history(GestureRollback::requiredBytes());
	PagePool localPool;
	GestureRollback localRollback{localPool};
	CHECK(Status::Ok == localPool.initialize(backing));
	CHECK(Status::Ok == localRollback.initialize(history));
	ResidentAudio localAudio{localPool, localRollback};
	Engine localEngine{localAudio, localRollback};
	Transpose localTranspose;
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		CHECK(Status::Ok == localTranspose.prepare(slot));
	AudioAdapter localAdapter{localEngine, localAudio, localPool, localTranspose};
	auto settings = defaultSettings();
	settings.startAction.mode = settings.stopAction.mode = ActionMode::Immediate;
	settings.tracks[0].source = Source::Stereo;
	settings.tracks[0].monitoring = Monitoring::Off;
	settings.tracks[0].transposeCents = 1200;
	CHECK(Status::Ok == localEngine.dispatch(Command::configure(settings), 0, clock));
	localTranspose.setCents(0, settings.tracks[0].transposeCents);
	CHECK(Status::Ok == localEngine.dispatch({CommandType::Press, 0}, 0, clock));
	constexpr uint32_t sampleRate = 48000;
	constexpr size_t phraseFrames = 4096;
	std::array<Frame, AudioAdapter::kMaxWindowFrames> input{}, output{};
	std::vector<Frame> source(phraseFrames), rendered;
	SampleClock now = 0;
	while (now < phraseFrames) {
		size_t count = std::min<size_t>(AudioAdapter::kMaxWindowFrames, phraseFrames - now);
		for (size_t i = 0; i < count; ++i) {
			const double phase = 2.0 * M_PI * 440.0 * double(now + i) / sampleRate;
			const auto sample = int32_t(std::sin(phase) * 1000000000.0);
			input[i] = {sample, sample};
			source[now + i] = input[i];
		}
		localAdapter.finalizeWindow(now, count, clock);
		localAdapter.renderDry(0, std::span(input).first(count), std::span(output).first(count));
		now += count;
	}
	CHECK(Status::Ok == localEngine.dispatch({CommandType::Press, 0}, now, clock));
	const auto checkpoint = localPool.sealedHandle(0);
	CHECK(checkpoint.valid());
	CHECK_EQUAL(source[137].l, localPool.frame(checkpoint, 137)->l);
	CHECK_EQUAL(source[137].r, localPool.frame(checkpoint, 137)->r);
	const auto end = now + sampleRate - phraseFrames;
	std::array<Frame, AudioAdapter::kMaxWindowFrames> silence{};
	while (now < end) {
		size_t count = std::min<size_t>(AudioAdapter::kMaxWindowFrames, end - now);
		localAdapter.finalizeWindow(now, count, clock);
		localAdapter.renderDry(0, std::span(silence).first(count), std::span(output).first(count));
		rendered.insert(rendered.end(), output.begin(), output.begin() + count);
		now += count;
	}
	unsigned crossings = 0;
	for (size_t i = 12000; i < rendered.size(); ++i)
		if (rendered[i - 1].l <= 0 && rendered[i].l > 0)
			++crossings;
	const auto measured = double(crossings) * sampleRate / double(rendered.size() - 12000);
	CHECK(measured > 800.0);
	CHECK(measured < 960.0);
}

TEST(LoopAudioAdapter, startingResidentPhraseResetsSelectedIdleMonitorHistory) {
	std::vector<std::byte> backing(8 * kPageBytes), history(GestureRollback::requiredBytes());
	PagePool localPool;
	GestureRollback localRollback{localPool};
	CHECK(Status::Ok == localPool.initialize(backing));
	CHECK(Status::Ok == localRollback.initialize(history));
	ResidentAudio localAudio{localPool, localRollback};
	Engine localEngine{localAudio, localRollback};
	Transpose localTranspose;
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		CHECK(Status::Ok == localTranspose.prepare(slot));
	AudioAdapter localAdapter{localEngine, localAudio, localPool, localTranspose};
	auto settings = defaultSettings();
	settings.startAction.mode = settings.stopAction.mode = ActionMode::Immediate;
	settings.tracks[0].source = Source::Stereo;
	settings.tracks[0].monitoring = Monitoring::On;
	settings.tracks[0].transposeCents = 1200;
	CHECK(Status::Ok == localEngine.dispatch(Command::configure(settings), 0, clock));
	localTranspose.setCents(0, 1200);
	CHECK(Status::Ok == localEngine.dispatch({CommandType::Select, 0}, 0, clock));
	std::array<Frame, AudioAdapter::kMaxWindowFrames> input{}, output{};
	SampleClock now = 0;
	for (; now < 2304; now += input.size()) {
		for (size_t i = 0; i < input.size(); ++i) {
			const double phase = 2.0 * M_PI * 440.0 * double(now + i) / 48000.0;
			const auto sample = int32_t(std::sin(phase) * 1000000000.0);
			input[i] = {sample, sample};
		}
		size_t count = input.size();
		localAdapter.finalizeWindow(now, count, clock);
		localAdapter.renderDry(0, input, output);
	}
	CHECK(Status::Ok == localEngine.dispatch({CommandType::Press, 0}, now, clock));
	size_t count = 4;
	localAdapter.finalizeWindow(now, count, clock);
	std::array<Frame, 4> takeInput{}, takeOutput{};
	takeInput.fill({100, 100});
	localAdapter.renderDry(0, takeInput, takeOutput);
	CHECK(Status::Ok == localEngine.dispatch({CommandType::Press, 0}, now + 4, clock));
	count = output.size();
	localAdapter.finalizeWindow(now + 4, count, clock);
	std::array<Frame, AudioAdapter::kMaxWindowFrames> silence{};
	localAdapter.renderDry(0, silence, output);
	for (const auto frame : output) {
		CHECK_EQUAL(100, frame.l);
		CHECK_EQUAL(100, frame.r);
	}
}

TEST(LoopAudioAdapter, oneShotRetriggerDiscardsThePreviousGrainStream) {
	std::vector<std::byte> backing(32 * kPageBytes), history(GestureRollback::requiredBytes());
	PagePool localPool;
	GestureRollback localRollback{localPool};
	CHECK(Status::Ok == localPool.initialize(backing));
	CHECK(Status::Ok == localRollback.initialize(history));
	ResidentAudio localAudio{localPool, localRollback};
	Engine localEngine{localAudio, localRollback};
	Transpose localTranspose;
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		CHECK(Status::Ok == localTranspose.prepare(slot));
	AudioAdapter localAdapter{localEngine, localAudio, localPool, localTranspose};
	auto settings = defaultSettings();
	settings.startAction.mode = settings.stopAction.mode = ActionMode::Immediate;
	settings.tracks[0].source = Source::Stereo;
	settings.tracks[0].monitoring = Monitoring::On;
	settings.tracks[0].playback = PlaybackMode::OneShot;
	settings.tracks[0].transposeCents = 1200;
	CHECK(Status::Ok == localEngine.dispatch(Command::configure(settings), 0, clock));
	localTranspose.setCents(0, 1200);
	CHECK(Status::Ok == localEngine.dispatch({CommandType::Select, 0}, 0, clock));
	CHECK(Status::Ok == localEngine.dispatch({CommandType::Press, 0}, 0, clock));
	constexpr size_t phraseFrames = 16384;
	std::array<Frame, AudioAdapter::kMaxWindowFrames> input{}, output{};
	SampleClock now = 0;
	while (now < phraseFrames) {
		const auto count = std::min<size_t>(AudioAdapter::kMaxWindowFrames, phraseFrames - now);
		input.fill({100, 100});
		size_t windowFrames = count;
		localAdapter.finalizeWindow(now, windowFrames, clock);
		localAdapter.renderDry(0, std::span(input).first(windowFrames), std::span(output).first(windowFrames));
		now += windowFrames;
	}
	CHECK(Status::Ok == localEngine.dispatch({CommandType::Press, 0}, now, clock));
	CHECK(localEngine.state(0).playing);
	for (size_t elapsed = 0; elapsed < GestureRollback::kProvisionalSamples; elapsed += input.size()) {
		input.fill({100000, 100000});
		size_t windowFrames = input.size();
		localAdapter.finalizeWindow(now, windowFrames, clock);
		localAdapter.renderDry(0, input, output);
		now += windowFrames;
	}
	CHECK(Status::Ok == localEngine.dispatch({CommandType::Press, 0}, now, clock));
	CHECK(localEngine.state(0).playing); // The one-shot restarted without a stopped edge.
	input.fill({0, 0});
	size_t windowFrames = input.size();
	localAdapter.finalizeWindow(now, windowFrames, clock);
	localAdapter.renderDry(0, input, output);
	CHECK_EQUAL(100, output[1].l);
	CHECK_EQUAL(100, output[1].r);
}

TEST(LoopAudioAdapter, physicalSourcesFollowRecorderMappingInCaptureAndMonitor) {
	for (auto source : {Source::Left, Source::Right, Source::Balanced, Source::Stereo}) {
		auto settings = engine.settings();
		settings.tracks[0].source = source;
		configure(settings);
		command(CommandType::Press, 0);
		window(0);
		Frame in[] = {{20, -10}}, out[1];
		adapter.renderDry(0, in, out);
		const Frame expected = source == Source::Left       ? Frame{20, 20}
		                       : source == Source::Right    ? Frame{-10, -10}
		                       : source == Source::Balanced ? Frame{15, 15}
		                                                    : Frame{20, -10};
		CHECK_EQUAL(expected.l, out[0].l);
		CHECK_EQUAL(expected.r, out[0].r);
		command(CommandType::Press, 1);
		window(1);
		Frame silence[1]{};
		adapter.renderDry(0, silence, out);
		CHECK_EQUAL(expected.l, out[0].l);
		CHECK_EQUAL(expected.r, out[0].r);
		command(CommandType::StopAll, 50000);
		command(CommandType::Press, 50001);
		command(CommandType::Release, 50001 + 2 * loop_station::kSampleRate);
	}
}

TEST(LoopAudioAdapter, capturingTracksOwnOverlappingPhysicalMonitorSources) {
	auto settings = engine.settings();
	settings.tracks[0].source = Source::Stereo;
	settings.tracks[1].source = Source::Left;
	settings.tracks[2].source = Source::Right;
	configure(settings);
	command(CommandType::Select, 0, 0);
	command(CommandType::Press, 0, 1);
	command(CommandType::Press, 0, 2);
	window(0);
	Frame in[] = {{10, 20}}, out[1];
	adapter.renderDry(0, in, out);
	CHECK_EQUAL(0, out[0].l);
	adapter.renderDry(1, in, out);
	CHECK_EQUAL(10, out[0].l);
	adapter.renderDry(2, in, out);
	CHECK_EQUAL(20, out[0].l);
	// Monitoring off still captures and suppresses the selected overlapping source.
	settings.tracks[1].monitoring = Monitoring::Off;
	configure(settings, 1);
	window(1);
	adapter.renderDry(0, in, out);
	CHECK_EQUAL(0, out[0].l);
	adapter.renderDry(1, in, out);
	CHECK_EQUAL(0, out[0].l);
}

TEST(LoopAudioAdapter, independentIdleSourceMonitorsAndUnselectedIdleIsSilent) {
	auto settings = engine.settings();
	settings.tracks[0].source = Source::Right;
	configure(settings);
	command(CommandType::Select, 0, 0);
	command(CommandType::Press, 0, 1); // LEFT
	window(0);
	Frame in[] = {{10, 20}}, out[1];
	adapter.renderDry(0, in, out);
	CHECK_EQUAL(20, out[0].l);
	adapter.renderDry(2, in, out);
	CHECK_EQUAL(0, out[0].l);
}

TEST(LoopAudioAdapter, allManualTracksShareOneFairAdmissionRound) {
	PagePool smallPool;
	std::array<std::byte, kPageBytes> onePage{};
	CHECK(Status::Ok == smallPool.initialize(onePage));
	GestureRollback smallRollback{smallPool};
	CHECK(Status::Ok == smallRollback.initialize(scratch));
	ResidentAudio smallAudio{smallPool, smallRollback};
	Engine smallEngine{smallAudio, smallRollback};
	Transpose smallTranspose;
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		CHECK(Status::Ok == smallTranspose.prepare(slot));
	AudioAdapter smallAdapter{smallEngine, smallAudio, smallPool, smallTranspose};
	auto settings = engine.settings();
	CHECK(Status::Ok == smallEngine.dispatch(Command::configure(settings), 0, clock));
	CHECK(Status::Ok == smallEngine.dispatch({CommandType::Press, 0}, 0, clock));
	CHECK(Status::Ok == smallEngine.dispatch({CommandType::Press, 1}, 0, clock));
	size_t count = 1;
	smallAdapter.finalizeWindow(0, count, clock);
	Frame in[] = {{10, 20}}, out[1];
	// Reverse output order cannot steal slot zero's fair grant.
	smallAdapter.renderDry(1, in, out);
	smallAdapter.renderDry(0, in, out);
	CHECK(Status::Ok == smallEngine.dispatch({CommandType::Press, 0}, 1, clock));
	CHECK_EQUAL(1, smallEngine.state(0).phraseFrames);
	CHECK_EQUAL(0, smallEngine.state(1).phraseFrames);
}

TEST(LoopAudioAdapter, tempoChangesDoNotResampleOrResetNativePhrasePhase) {
	record();
	window(4, 3);
	Frame in[3]{}, out[3];
	adapter.renderDry(0, in, out);
	CHECK_EQUAL(3, adapter.phase(0));
	clock.setTempo(7, 240);
	window(7, 3);
	adapter.renderDry(0, in, out);
	CHECK_EQUAL(2, adapter.phase(0));
	for (auto frame : out)
		CHECK_EQUAL(100, frame.l);
}

TEST(LoopAudioAdapter, monitorFadeDoesNotChangeCaptureOrBacking) {
	command(CommandType::Press, 0);
	window(0, 4);
	adapter.setMonitoringGain(INT32_MAX, 0);
	Frame in[4] = {{801, 801}, {801, 801}, {801, 801}, {801, 801}}, out[4];
	adapter.renderDry(0, in, out);
	CHECK_EQUAL(600, out[0].l);
	CHECK_EQUAL(400, out[1].l);
	CHECK_EQUAL(200, out[2].l);
	CHECK_EQUAL(0, out[3].l);
	command(CommandType::Press, 4);
	window(4, 4);
	adapter.renderDry(0, in, out);
	for (auto frame : out)
		CHECK_EQUAL(801, frame.l);
}

TEST(LoopAudioAdapter, closedCaptureGateHasNoAlignmentTail) {
	record();
	command(CommandType::Press, 20000);
	window(20000);
	Frame in[] = {{10, 10}}, out[1];
	adapter.renderDry(0, in, out);
	command(CommandType::Press, 40000);
	window(40000, 4);
	Frame loud[4] = {{1000, 1000}, {1000, 1000}, {1000, 1000}, {1000, 1000}}, pass[4];
	adapter.renderDry(0, loud, pass);
	window(40004, 4);
	Frame silence[4]{};
	adapter.renderDry(0, silence, pass);
	CHECK_EQUAL(100, pass[0].l);
	CHECK_EQUAL(100, pass[1].l);
	CHECK_EQUAL(100, pass[2].l);
	CHECK_EQUAL(110, pass[3].l);
}

TEST(LoopAudioAdapter, audibleMixSaturatesAndInvalidSlotsAreSilent) {
	record(0, {INT32_MAX, INT32_MAX});
	command(CommandType::Press, 20000);
	window(20000);
	Frame in[] = {{100, 100}}, out[2];
	adapter.renderDry(0, in, out);
	CHECK_EQUAL(INT32_MAX, out[0].l);
	CHECK_EQUAL(0, out[1].l);
	adapter.renderDry(kInvalidSlot, in, out);
	CHECK_EQUAL(0, out[0].l);
	CHECK_EQUAL(0, out[1].r);
}

TEST(LoopAudioAdapter, firmwareStereoProviderUsesSameDryRoutingAndPhase) {
	command(CommandType::Press, 0);
	window(0, 2);
	StereoSample input[2] = {{10, 30}, {20, 40}}, output[2];
	adapter.renderDry(0, std::span<const StereoSample>(input), std::span(output));
	CHECK_EQUAL(10, output[0].r);
	CHECK_EQUAL(20, output[1].l);
	command(CommandType::Press, 2);
	window(2, 2);
	StereoSample silence[2]{};
	adapter.renderDry(0, std::span<const StereoSample>(silence), std::span(output));
	CHECK_EQUAL(10, output[0].r);
	CHECK_EQUAL(20, output[1].l);
	CHECK_EQUAL(0, adapter.phase(0));
}

TEST(LoopAudioAdapter, repeatedSourceRenderInOneDeviceWindowDoesNotAdvanceOrCaptureTwice) {
	record();
	command(CommandType::Press, 20000);
	window(20000);
	Frame in[] = {{10, 10}}, first[1], second[1];
	adapter.renderDry(0, in, first);
	adapter.renderDry(0, in, second);
	CHECK_EQUAL(1, adapter.phase(0));
	CHECK_EQUAL(110, second[0].l);
	CHECK_EQUAL(first[0].l, second[0].l);
	command(CommandType::Press, 40000);
	window(40000, 4);
	Frame silence[4]{}, pass[4];
	adapter.renderDry(0, silence, pass);
	CHECK_EQUAL(100, pass[0].l);
	CHECK_EQUAL(110, pass[3].l);
}

TEST(LoopAudioAdapter, capacitySplitsDeviceWindowAndImmediatelyPlaysOrDubsWithoutLosingTail) {
	for (auto action : {RecordAction::REC_PLAY_DUB, RecordAction::REC_DUB_PLAY}) {
		for (auto monitor : {Monitoring::On, Monitoring::Off}) {
			std::array<std::byte, kPageBytes> storage{};
			PagePool pool;
			GestureRollback rollback{pool};
			std::vector<std::byte> scratch(GestureRollback::requiredBytes());
			CHECK(Status::Ok == pool.initialize(storage));
			CHECK(Status::Ok == rollback.initialize(scratch));
			ResidentAudio audio{pool, rollback};
			Engine engine{audio, rollback};
			Transpose transpose;
			for (SlotId slot = 0; slot < kTrackCount; ++slot)
				CHECK(Status::Ok == transpose.prepare(slot));
			AudioAdapter adapter{engine, audio, pool, transpose};
			auto settings = defaultSettings();
			settings.startAction.mode = settings.stopAction.mode = ActionMode::Immediate;
			settings.recordAction = action;
			settings.tracks[0].monitoring = monitor;
			CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 0, clock));
			CHECK(Status::Ok == engine.dispatch({CommandType::Press, 0}, 0, clock));
			Frame input[128], output[128];
			std::fill_n(input, 128, Frame{10, 10});
			SampleClock now = 0;
			while (now < 500) {
				size_t count = std::min<SampleClock>(128, 500 - now);
				adapter.finalizeWindow(now, count, clock);
				adapter.renderDry(0, std::span(input).first(count), std::span(output).first(count));
				now += count;
			}
			size_t count = 128;
			adapter.finalizeWindow(500, count, clock);
			CHECK_EQUAL(12, count);
			adapter.renderDry(0, std::span(input).first(count), std::span(output).first(count));
			CHECK_EQUAL(monitor == Monitoring::On ? 10 : 0, output[0].l);
			count = 116; // Entire remainder of the original device window survives.
			adapter.finalizeWindow(512, count, clock);
			CHECK_EQUAL(116, count);
			CHECK(engine.state(0).playing);
			CHECK_EQUAL(512, engine.state(0).phraseFrames);
			CHECK(Status::NoMemory == engine.state(0).error);
			const bool dub = action == RecordAction::REC_DUB_PLAY;
			CHECK_EQUAL(dub, engine.state(0).capture);
			adapter.renderDry(0, std::span(input).first(count), std::span(output).first(count));
			for (size_t i = 0; i < count; ++i)
				CHECK_EQUAL(dub && monitor == Monitoring::On ? 20 : 10, output[i].l);
			CHECK_EQUAL(116, adapter.phase(0));
			CHECK(Status::Ok == engine.dispatch({CommandType::StopAll}, 628, clock));
			CHECK(Status::Ok == engine.dispatch({CommandType::Acknowledge, 0}, 628, clock));
			CHECK(Status::Ok == engine.dispatch({CommandType::StartAll}, 628, clock));
			count = 1;
			adapter.finalizeWindow(628, count, clock);
			Frame silence[1]{};
			adapter.renderDry(0, silence, std::span(output).first(1));
			CHECK_EQUAL(dub ? 20 : 10, output[0].l);
		}
	}
}

TEST(LoopAudioAdapter, exhaustedInitialCapacityIsServicedBeforeAnyPostBoundarySample) {
	std::array<std::byte, kPageBytes> storage{};
	PagePool pool;
	GestureRollback rollback{pool};
	std::vector<std::byte> scratch(GestureRollback::requiredBytes());
	CHECK(Status::Ok == pool.initialize(storage));
	CHECK(Status::Ok == rollback.initialize(scratch));
	ResidentAudio audio{pool, rollback};
	Engine engine{audio, rollback};
	Transpose transpose;
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		CHECK(Status::Ok == transpose.prepare(slot));
	AudioAdapter adapter{engine, audio, pool, transpose};
	auto settings = engine.settings();
	settings.startAction.mode = ActionMode::Immediate;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 0, clock));
	CHECK(Status::Ok == engine.dispatch({CommandType::Press, 0}, 0, clock));
	Frame in[128], out[128];
	std::fill_n(in, 128, Frame{7, 7});
	for (SampleClock now = 0; now < 512; now += 128) {
		size_t count = 128;
		adapter.finalizeWindow(now, count, clock);
		adapter.renderDry(0, in, out);
	}
	size_t count = 128;
	adapter.finalizeWindow(512, count, clock);
	CHECK_EQUAL(128, count);
	CHECK(engine.state(0).playing);
	Frame silence[128]{};
	adapter.renderDry(0, silence, out);
	CHECK_EQUAL(7, out[0].l);
	CHECK_EQUAL(128, adapter.phase(0));
}

TEST(LoopAudioAdapter, durationSettingChangeDoesNotReplaceAnActiveTakesAdmissionPolicy) {
	command(CommandType::Press, 0);
	window(0, 1);
	Frame in[] = {{10, 10}}, out[1];
	adapter.renderDry(0, in, out);
	Command duration{CommandType::SetDuration, 0};
	duration.durationBars = 1;
	CHECK(Status::Ok == engine.dispatch(duration, 1, clock));
	window(1, 1);
	CHECK_FALSE(engine.state(0).hasPhrase);
	CHECK(engine.state(0).capture);
	adapter.renderDry(0, in, out);
	command(CommandType::Press, 2);
	CHECK_EQUAL(2, engine.state(0).phraseFrames);
}
TEST(LoopAudioAdapter, stalledArchiveLeavesRenderSamplesAndCommandsOnTheAudioOwner) {
	struct BlockedStorage : ArchiveStorage {
		std::function<void()> audioWhileBlocked;
		Status begin(SlotId, Generation) override { return Status::Ok; }
		Status write(std::span<const std::byte>, size_t& written) override {
			written = 0;
			if (audioWhileBlocked) {
				auto runAudio = std::exchange(audioWhileBlocked, {});
				runAudio();
			}
			return Status::Busy;
		}
		Status finalize() override { return Status::Busy; }
		void abort() override {}
		void discard(SlotId, Generation) override {}
	} blocked;
	std::vector<std::byte> pages(4 * kPageBytes);
	SnapshotStore snapshots{pool, rollback};
	CHECK(Status::Ok == snapshots.initialize(pages));
	audio.setWriteObservers(nullptr, &snapshots);
	Archive archive{pool, rollback, snapshots};
	archive.setStorage(&blocked);
	record();
	CHECK(Status::Ok == archive.request(0, pool.sealedHandle(0).generation));
	blocked.audioWhileBlocked = [&] {
		for (unsigned i = 0; i < 100; ++i) {
			window(1000 + i * 128, 128);
			std::array<Frame, 128> input{}, out{};
			adapter.renderDry(0, input, out);
			for (const auto& frame : out) {
				CHECK_EQUAL(100, frame.l);
				CHECK_EQUAL(100, frame.r);
			}
			CHECK(engine.state(0).playing);
			CHECK_FALSE(engine.state(0).capture);
		}
		command(CommandType::Press, 20000);
		CHECK(engine.state(0).capture);
		window(20000);
		Frame in{256, 999}, out;
		adapter.renderDry(0, {&in, 1}, {&out, 1});
		CHECK_EQUAL(356, out.l);
		CHECK_EQUAL(356, out.r);
		command(CommandType::Press, 40000);
		CHECK_FALSE(engine.state(0).capture);
		CHECK(engine.state(0).playing);
		command(CommandType::StopAll, 40001);
		CHECK_FALSE(engine.state(0).playing);
	};
	for (unsigned i = 0; i < 10; ++i)
		archive.service(4096);
	CHECK(ArchiveState::Writing == archive.state(0));
	archive.cancel(0);
	for (unsigned i = 0; i < 5; ++i)
		archive.service(4096);
	CHECK(archive.idle());
	CHECK_FALSE(engine.state(0).capture);
	CHECK_FALSE(engine.state(0).playing);
	CHECK(engine.state(0).error == Status::Ok);
}
