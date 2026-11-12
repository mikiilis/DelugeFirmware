#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/engine.h"
#include <array>
#include <vector>
using namespace loop_station;
TEST_GROUP(LoopEngine) {
	std::vector<std::byte> backing, scratch;
	PagePool pool;
	GestureRollback rollback{pool};
	ResidentAudio audio{pool, rollback};
	Engine engine{audio, rollback};
	ClockMap clock;
	void setup() {
		backing.resize(400 * kPageBytes);
		scratch.resize(GestureRollback::requiredBytes());
		CHECK(Status::Ok == pool.initialize(backing));
		CHECK(Status::Ok == rollback.initialize(scratch));
		auto settings = defaultSettings();
		settings.startAction.mode = ActionMode::Immediate;
		settings.stopAction.mode = ActionMode::Immediate;
		CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 0, clock));
	}
	void command(CommandType type, SampleClock now, SlotId slot = 0) {
		CHECK(Status::Ok == engine.dispatch({type, slot}, now, clock));
	}
	void samples(unsigned count, SlotId slot = 0, Frame value = {10, -20}) {
		std::array<Frame, 128> in, out;
		in.fill(value);
		while (count) {
			auto n = std::min<unsigned>(count, 128);
			CHECK(Status::Ok == pool.beginRenderRound(0xff));
			audio.render(slot, std::span(in).first(n), std::span(out).first(n));
			count -= n;
		}
	}
	void phrase(bool oneShot = false, RecordAction action = RecordAction::REC_PLAY_DUB, SlotId slot = 0) {
		auto settings = engine.settings();
		settings.tracks[slot].playback = oneShot ? PlaybackMode::OneShot : PlaybackMode::Loop;
		settings.recordAction = action;
		CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 0, clock));
		command(CommandType::Press, 0, slot);
		samples(8, slot);
		command(CommandType::Press, 8, slot);
	}
};
TEST(LoopEngine, baselineEmptyRecordPlayDubAndSelection) {
	command(CommandType::Select, 0, 3);
	CHECK_EQUAL(3, engine.selected());
	command(CommandType::Select, 0, 3);
	CHECK_EQUAL(kInvalidSlot, engine.selected());
	command(CommandType::ShiftPress, 0);
	CHECK_FALSE(engine.state(0).capture);
	phrase();
	auto s = engine.state(0);
	CHECK(s.hasPhrase);
	CHECK(s.playing);
	CHECK_FALSE(s.capture);
	CHECK_EQUAL(8, s.phraseFrames);
	command(CommandType::Press, 20000);
	CHECK(engine.state(0).capture);
	CHECK(rollback.provisional(0));
	samples(3);
	CHECK_EQUAL(3, audio.position(0));
	command(CommandType::Press, 40000);
	CHECK_FALSE(engine.state(0).capture);
	CHECK(engine.state(0).playing);
	CHECK_EQUAL(3, audio.position(0));
}
TEST(LoopEngine, pendingTapCancelsOnlyUserActionAndServicesAlreadyDueFirst) {
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 0, clock));
	command(CommandType::Press, 0);
	CHECK(engine.state(0).pending);
	CHECK_FALSE(engine.state(0).capture);
	CHECK_EQUAL(22050, *engine.nextActionSample(clock));
	command(CommandType::Press, 1);
	CHECK_FALSE(engine.state(0).pending);
	CHECK_FALSE(engine.nextActionSample(clock));
	command(CommandType::Press, 2);
	command(CommandType::Press, 22050); // due start first; press now completes zero-frame recording
	CHECK_FALSE(engine.state(0).capture);
	CHECK_FALSE(engine.state(0).hasPhrase);
}
TEST(LoopEngine, musicalDurationIsIndependentOfCancelledUserStopAndTempoEdit) {
	auto settings = engine.settings();
	settings.tracks[0].recordDurationBars = 1;
	settings.stopAction = {ActionMode::Synced, Interval::Bar};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 0, clock));
	command(CommandType::Press, 0);
	samples(100);
	command(CommandType::Press, 100);
	command(CommandType::Press, 101);
	CHECK_FALSE(engine.state(0).pending);
	CHECK(engine.state(0).capture);
	clock.setTempo(44100, 60);
	CHECK_EQUAL(132300, *engine.nextActionSample(clock));
	engine.service(132300, clock);
	CHECK_FALSE(engine.state(0).capture);
	CHECK(engine.state(0).playing);
	CHECK_EQUAL(100, engine.state(0).phraseFrames);
}
TEST(LoopEngine, one_shot_record_actions_and_explicit_stop) {
	phrase(true);
	samples(8);
	engine.service(16, clock);
	CHECK_FALSE(engine.state(0).playing);
	command(CommandType::Press, 20000);
	CHECK_FALSE(engine.state(0).playing);
	command(CommandType::Release, 20001);
	CHECK(engine.state(0).playing);
	CHECK_EQUAL(0, audio.position(0));
	samples(3);
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	settings.stopAction = {ActionMode::Synced, Interval::Bar};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20004, clock));
	command(CommandType::Press, 40000);
	CHECK(engine.state(0).pending);
	CHECK_EQUAL(3, audio.position(0));
	engine.service(44100, clock);
	CHECK_EQUAL(0, audio.position(0));
	CHECK(engine.state(0).playing);
	// Independent slot uses automatic, non-provisional repeating overdub.
	settings.recordAction = RecordAction::REC_DUB_PLAY;
	settings.startAction.mode = ActionMode::Immediate;
	settings.tracks[1].playback = PlaybackMode::OneShot;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 44100, clock));
	command(CommandType::Press, 44100, 1);
	samples(8, 1);
	command(CommandType::Press, 44108, 1); // queues initial completion
	engine.service(132300, clock);
	CHECK(engine.state(1).capture);
	CHECK_FALSE(rollback.provisional(1));
	samples(27, 1);
	CHECK_EQUAL(3, audio.position(1));
	command(CommandType::Press, 132327, 1);
	CHECK_FALSE(engine.state(1).capture);
	CHECK_FALSE(engine.state(1).pending);
	CHECK_EQUAL(3, audio.position(1));
	samples(5, 1);
	engine.service(132332, clock);
	CHECK_FALSE(engine.state(1).playing);
}
TEST(LoopEngine, oneShotGateCloseAtZeroCompletesFullPass) {
	phrase(true, RecordAction::REC_DUB_PLAY);
	samples(24);
	command(CommandType::Press, 20000);
	CHECK_EQUAL(0, audio.position(0));
	samples(7);
	engine.service(20007, clock);
	CHECK(engine.state(0).playing);
	samples(1);
	engine.service(20008, clock);
	CHECK_FALSE(engine.state(0).playing);
}
TEST(LoopEngine, doubleTapRestoresProvisionalAudioAndQueuesStop) {
	phrase();
	auto settings = engine.settings();
	settings.stopAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::Press, 20000);
	samples(3, 0, {100, 100});
	command(CommandType::Press, 20003);
	CHECK_FALSE(engine.state(0).capture);
	CHECK(engine.state(0).pending);
	// Reach an actually modified frame, rather than the untouched frame three.
	samples(5, 0, {});
	Frame input[1]{};
	Frame output[1]{};
	audio.render(0, input, output);
	CHECK_EQUAL(10, output[0].l);
	engine.service(22050, clock);
	CHECK_FALSE(engine.state(0).playing);
	CHECK_FALSE(engine.state(0).pending);
}
TEST(LoopEngine, expiredDoubleTapDoesNotDiscardConfirmedSession) {
	phrase();
	command(CommandType::Press, 20000);
	samples(2, 0, {100, 100});
	command(CommandType::Press, 35435);
	CHECK_FALSE(engine.state(0).capture);
	CHECK_FALSE(engine.state(0).pending);
	CHECK(engine.state(0).playing);
	samples(6);
	Frame in[1]{};
	Frame out[1]{};
	audio.render(0, in, out);
	CHECK_EQUAL(110, out[0].l);
}
TEST(LoopEngine, stoppedReleaseStartsAndHoldReleaseClearsWithSettingsRetained) {
	phrase();
	command(CommandType::TransportStop, 9);
	command(CommandType::Press, 100);
	CHECK_EQUAL(0, engine.heldDurationSamples(0, 100));
	CHECK_EQUAL(44100, engine.heldDurationSamples(0, 44200));
	CHECK_EQUAL(88200, engine.heldDurationSamples(0, 88300));
	engine.service(88300, clock);
	CHECK(engine.state(0).hasPhrase);
	CHECK_FALSE(engine.state(0).playing);
	command(CommandType::Release, 88300);
	CHECK_EQUAL(0, engine.heldDurationSamples(0, 88300));
	CHECK_FALSE(engine.state(0).hasPhrase);
	CHECK(Status::Ok == pool.beginRenderRound(0));
	pool.serviceRetirement(16);
	CHECK_EQUAL(400, pool.freePages());
	phrase();
	command(CommandType::TransportStop, 9);
	command(CommandType::Press, 100);
	CHECK_FALSE(engine.state(0).playing);
	command(CommandType::Release, 101);
	CHECK(engine.state(0).playing);
	CHECK_EQUAL(0, audio.position(0));
}
TEST(LoopEngine, stopAllCoversCapturesCancelsStartsAndSharesDeadline) {
	phrase(false, RecordAction::REC_DUB_PLAY);
	command(CommandType::Press, 20000, 1);
	samples(5, 1);
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	settings.stopAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20005, clock));
	command(CommandType::Press, 20005, 2);
	command(CommandType::StopAll, 20006);
	CHECK_FALSE(engine.state(0).capture);
	CHECK(engine.state(0).playing);
	CHECK(engine.state(1).capture);
	CHECK_FALSE(engine.state(2).pending);
	CHECK_EQUAL(22050, *engine.nextActionSample(clock));
	engine.service(22050, clock);
	CHECK_FALSE(engine.state(0).playing);
	CHECK_FALSE(engine.state(1).capture);
	CHECK_FALSE(engine.state(1).playing);
	CHECK(engine.state(1).hasPhrase);
	command(CommandType::StartAll, 22050);
	CHECK_EQUAL(44100, *engine.nextActionSample(clock));
	engine.service(44100, clock);
	CHECK(engine.state(0).playing);
	CHECK(engine.state(1).playing);
	CHECK_FALSE(engine.state(0).capture);
}
TEST(LoopEngine, exitClosesCaptureCancelsStartsAndUsesInitialStopAction) {
	phrase(false, RecordAction::REC_DUB_PLAY);
	command(CommandType::Press, 20000, 1);
	samples(5, 1);
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	settings.stopAction = settings.startAction;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20005, clock));
	command(CommandType::Press, 20005, 2);
	command(CommandType::Exit, 20006);
	CHECK_FALSE(engine.state(0).capture);
	CHECK(engine.state(0).playing);
	CHECK(engine.state(1).capture);
	CHECK_FALSE(engine.state(2).pending);
	engine.service(22050, clock);
	CHECK_FALSE(engine.state(1).capture);
	CHECK(engine.state(1).playing);
}
TEST(LoopEngine, transportStopsImmediatelyRetainsTakesAndCancelsEverything) {
	phrase(false, RecordAction::REC_DUB_PLAY);
	command(CommandType::Press, 20000, 1);
	samples(4, 1);
	command(CommandType::TransportStop, 20004);
	for (SlotId s = 0; s < 8; ++s) {
		CHECK_FALSE(engine.state(s).capture);
		CHECK_FALSE(engine.state(s).playing);
		CHECK_FALSE(engine.state(s).pending);
	}
	CHECK(engine.state(0).hasPhrase);
	CHECK(engine.state(1).hasPhrase);
	CHECK_FALSE(engine.nextActionSample(clock));
}
TEST(LoopEngine, captureErrorIsLocalAndFirstPressOnlyAcknowledges) {
	phrase(false, RecordAction::REC_DUB_PLAY);
	Command error{CommandType::CaptureError, 0};
	error.status = Status::SourceUnavailable;
	CHECK(Status::SourceUnavailable == engine.dispatch(error, 20000, clock));
	CHECK_FALSE(engine.state(0).capture);
	CHECK(engine.state(0).playing);
	CHECK(engine.state(0).error == Status::SourceUnavailable);
	command(CommandType::Press, 20001);
	CHECK(engine.state(0).error == Status::Ok);
	CHECK_FALSE(engine.state(0).capture);
	command(CommandType::Press, 20002);
	CHECK(engine.state(0).capture);
}
TEST(LoopEngine, capacityCompletionTransitionsImmediatelyAndReportsMemoryLimit) {
	// Reserve nearly all pages with the other slot, leaving one 512-frame manual page.
	CHECK(Status::Ok == pool.beginPhrase(7, 399 * 512));
	command(CommandType::Press, 0);
	samples(513);
	engine.service(513, clock);
	CHECK(engine.state(0).hasPhrase);
	CHECK_EQUAL(512, engine.state(0).phraseFrames);
	CHECK(engine.state(0).playing);
	CHECK_FALSE(engine.state(0).capture);
	CHECK(engine.state(0).error == Status::NoMemory);
}
TEST(LoopEngine, invalidCommandsAndConfigurationDoNotMutateActiveState) {
	CHECK(Status::Invalid == engine.dispatch({CommandType::Press, 8}, 0, clock));
	phrase();
	auto settings = engine.settings();
	settings.tracks[0].recordDurationBars = 1;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	CHECK_EQUAL(8, engine.state(0).phraseFrames);
}

TEST(LoopEngine, slowdownAtAdmittedCapacityReportsMemoryLimitAndPreservesFrames) {
	auto settings = engine.settings();
	settings.tracks[0].recordDurationBars = 1;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 0, clock));
	command(CommandType::Press, 0);
	samples(44100);
	clock.setTempo(44100, 60);
	CHECK_EQUAL(132300, *engine.nextActionSample(clock));
	samples(44100);
	engine.service(88200, clock);
	CHECK_FALSE(engine.state(0).capture);
	CHECK(engine.state(0).playing);
	CHECK_EQUAL(88200, engine.state(0).phraseFrames);
	CHECK(engine.state(0).error == Status::NoMemory);
}
TEST(LoopEngine, oneShotExplicitSyncedStopWaitsForStopActionAndCanBeCancelled) {
	phrase(true, RecordAction::REC_DUB_PLAY);
	auto settings = engine.settings();
	settings.stopAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::StopAll, 20000);
	CHECK_FALSE(engine.state(0).capture);
	samples(24);
	engine.service(20024, clock);
	CHECK(engine.state(0).playing);
	CHECK(engine.state(0).pending);
	command(CommandType::Press, 20025);
	CHECK_FALSE(engine.state(0).pending);
	CHECK_FALSE(engine.state(0).capture);
	samples(8);
	engine.service(20033, clock);
	CHECK_FALSE(engine.state(0).playing);
}
TEST(LoopEngine, oneShotDoubleTapStopsOnExplicitDeadlineAndExitUsesNativeBoundary) {
	phrase(true, RecordAction::REC_DUB_PLAY);
	auto settings = engine.settings();
	settings.stopAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::Press, 20000);
	samples(2);
	command(CommandType::Press, 20002);
	CHECK(engine.state(0).pending);
	samples(22);
	engine.service(20024, clock);
	CHECK(engine.state(0).playing);
	engine.service(22050, clock);
	CHECK_FALSE(engine.state(0).playing);
	command(CommandType::Press, 30000);
	command(CommandType::Release, 30001);
	// Playing one-shots use retrigger rather than an overdub gesture.
	command(CommandType::Exit, 30001);
	samples(8);
	engine.service(30009, clock);
	CHECK_FALSE(engine.state(0).playing);
}
TEST(LoopEngine, errorAcknowledgementReleaseCannotRestartStoppedPhrase) {
	phrase();
	command(CommandType::TransportStop, 20000);
	Command error{CommandType::CaptureError, 0};
	error.status = Status::SourceUnavailable;
	CHECK(Status::SourceUnavailable == engine.dispatch(error, 20001, clock));
	command(CommandType::Press, 20002);
	command(CommandType::Release, 20003);
	CHECK_FALSE(engine.state(0).playing);
	CHECK_FALSE(engine.state(0).pending);
	command(CommandType::Press, 20004);
	command(CommandType::Release, 20005);
	CHECK(engine.state(0).playing);
}
TEST(LoopEngine, eightSlotsStartTogetherWithoutRetriggeringActivePlayback) {
	for (SlotId id = 0; id < 8; ++id)
		phrase(false, RecordAction::REC_PLAY_DUB, id);
	command(CommandType::TransportStop, 20000);
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::StartAll, 20001);
	CHECK_EQUAL(22050, *engine.nextActionSample(clock));
	for (SlotId id = 0; id < 8; ++id)
		CHECK(engine.state(id).pending);
	engine.service(22050, clock);
	for (SlotId id = 0; id < 8; ++id) {
		CHECK(engine.state(id).playing);
		samples(3, id);
	}
	command(CommandType::StartAll, 22053);
	CHECK_FALSE(engine.nextActionSample(clock));
	for (SlotId id = 0; id < 8; ++id)
		CHECK_EQUAL(3, audio.position(id));
}
TEST(LoopEngine, fixedDurationAdmissionFailureIsSlotLocalAndRequiresAcknowledgement) {
	auto settings = engine.settings();
	settings.tracks[0].recordDurationBars = 10;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 0, clock));
	CHECK(Status::NoMemory == engine.dispatch({CommandType::Press, 0}, 0, clock));
	CHECK(engine.state(0).error == Status::NoMemory);
	CHECK_FALSE(engine.state(0).capture);
	command(CommandType::Press, 1, 1);
	CHECK(engine.state(1).capture);
	command(CommandType::Press, 2);
	CHECK(engine.state(0).error == Status::Ok);
	CHECK_FALSE(engine.state(0).capture);
}
TEST(LoopEngine, durationCommandAffectsNextTakeWithoutRemovingCurrentDuration) {
	Command duration{CommandType::SetDuration, 0};
	duration.durationBars = 1;
	CHECK(Status::Ok == engine.dispatch(duration, 0, clock));
	command(CommandType::Press, 0);
	samples(8);
	duration.durationBars = 0;
	CHECK(Status::Ok == engine.dispatch(duration, 8, clock));
	CHECK_EQUAL(88200, *engine.nextActionSample(clock));
	engine.service(88200, clock);
	CHECK(engine.state(0).playing);
	CHECK_EQUAL(0, engine.settings().tracks[0].recordDurationBars);
}
TEST(LoopEngine, musicalStartWorksBeyondFirmwareTimerWrapAndWhileTransportStopped) {
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 4294967290ULL, clock));
	command(CommandType::Press, 4294967290ULL);
	CHECK_EQUAL(4294987200ULL, *engine.nextActionSample(clock));
	engine.service(4294987199ULL, clock);
	CHECK_FALSE(engine.state(0).capture);
	engine.service(4294987200ULL, clock);
	CHECK(engine.state(0).capture);
}

// Cancelling the last possible sample must switch the effective audio without
// draining all eight scratch arenas synchronously in command dispatch.
TEST(LoopEngine, eightLastSampleDoubleTapsBoundDispatchAndFairRetirement) {
	for (SlotId slot = 0; slot < 8; ++slot) {
		command(CommandType::Press, 0, slot);
		samples(16000, slot);
		command(CommandType::Press, 16000, slot);
	}
	auto settings = engine.settings();
	settings.stopAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	for (SlotId slot = 0; slot < 8; ++slot)
		command(CommandType::Press, 20000, slot);
	for (SlotId slot = 0; slot < 8; ++slot)
		samples(15434, slot, {100, -100});
	for (SlotId slot = 0; slot < 8; ++slot) {
		const auto before = rollback.work();
		command(CommandType::Press, 35434, slot);
		CHECK(rollback.work().retiredFrames - before.retiredFrames <= 128);
		CHECK(rollback.work().treeSteps - before.treeSteps <= 32768);
		CHECK_FALSE(engine.state(slot).capture);
		CHECK(engine.state(slot).pending);
		CHECK(engine.state(slot).playing);
	}
	CHECK_EQUAL(44100, *engine.nextActionSample(clock));
	// Playback visits modified frames while retirement is still pending.
	for (SlotId slot = 0; slot < 8; ++slot) {
		samples(566, slot, {});
		Frame input{}, output;
		audio.render(slot, {&input, 1}, {&output, 1});
		CHECK_EQUAL(10, output.l);
		CHECK_EQUAL(-20, output.r);
	}
	for (unsigned credit = 0; credit < 123480; credit += 64) {
		const auto before = rollback.work();
		const auto count = std::min(64u, 123480u - credit);
		rollback.service(count);
		CHECK(rollback.work().retiredFrames - before.retiredFrames <= count);
		CHECK(rollback.work().treeSteps - before.treeSteps <= 256 * count);
	}
	CHECK_EQUAL(123472, rollback.work().restoredFrames);
	for (SlotId slot = 0; slot < 8; ++slot)
		CHECK_FALSE(rollback.pending(slot));
}

// A firmware timer wrap during a gesture must preserve both sides of the
// 15435-sample cutoff, rather than expiring or extending the gesture on wrap.
TEST(LoopEngine, extendedTimerWrapPreservesLastSampleCancelAndExactExpiry) {
	phrase(false, RecordAction::REC_PLAY_DUB, 0);
	phrase(false, RecordAction::REC_PLAY_DUB, 1);
	SampleClockExtender timer;
	const auto begin = timer.observe(0xffffff00);
	CHECK_EQUAL(4294967040ULL, begin);
	command(CommandType::Press, begin, 0);
	command(CommandType::Press, begin, 1);
	samples(3, 0, {100, -100});
	samples(3, 1, {100, -100});
	const auto last = timer.observe(15178);
	CHECK_EQUAL(4294982474ULL, last);
	command(CommandType::Press, last, 0);
	CHECK_FALSE(engine.state(0).playing);
	CHECK(rollback.provisional(1));
	const auto expired = timer.observe(15179);
	CHECK_EQUAL(4294982475ULL, expired);
	command(CommandType::Press, expired, 1);
	CHECK(engine.state(1).playing);
	CHECK_FALSE(engine.state(1).capture);
	CHECK_FALSE(rollback.provisional(1));
	CHECK_EQUAL(3, rollback.work().restoredFrames);
	command(CommandType::Press, expired + 1, 0);
	command(CommandType::Release, expired + 2, 0);
	Frame input{}, output;
	audio.render(0, {&input, 1}, {&output, 1});
	CHECK_EQUAL(10, output.l);
	samples(5, 1, {});
	audio.render(1, {&input, 1}, {&output, 1});
	CHECK_EQUAL(110, output.l);
	CHECK_EQUAL(-120, output.r);
}
TEST(LoopEngine, stoppedPendingStartCancelsOnPressAndReleaseDoesNothing) {
	phrase();
	command(CommandType::TransportStop, 20000);
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::Press, 20001);
	command(CommandType::Release, 20002);
	CHECK(engine.state(0).pending);
	command(CommandType::Press, 20003);
	command(CommandType::Release, 20004);
	CHECK_FALSE(engine.state(0).pending);
	CHECK_FALSE(engine.state(0).playing);
}
TEST(LoopEngine, oneShotCaptureFailureClosesOnlyAffectedGateAndKeepsNativeBoundary) {
	phrase(true, RecordAction::REC_DUB_PLAY);
	command(CommandType::Press, 20000, 1);
	samples(3, 0);
	Command error{CommandType::CaptureError, 0};
	error.status = Status::SourceUnavailable;
	CHECK(Status::SourceUnavailable == engine.dispatch(error, 20003, clock));
	CHECK_FALSE(engine.state(0).capture);
	CHECK(engine.state(0).playing);
	CHECK(engine.state(1).capture);
	samples(5, 0);
	engine.service(20008, clock);
	CHECK_FALSE(engine.state(0).playing);
	CHECK(engine.state(0).error == Status::SourceUnavailable);
}
TEST(LoopEngine, clearWaitsForBoundedRollbackRetirementBeforeReleasingPhrase) {
	command(CommandType::Press, 0);
	samples(1024);
	command(CommandType::Press, 1024);
	command(CommandType::Press, 20000);
	samples(1024);
	command(CommandType::Press, 21024);
	CHECK_FALSE(engine.state(0).playing);
	CHECK(rollback.pending(0));
	command(CommandType::Press, 21025);
	command(CommandType::Release, 109225);
	CHECK(engine.state(0).pending);
	CHECK(engine.state(0).hasPhrase);
	for (SampleClock now = 109226; now < 109250; ++now)
		engine.service(now, clock);
	CHECK_FALSE(engine.state(0).pending);
	CHECK_FALSE(engine.state(0).hasPhrase);
	CHECK_FALSE(rollback.pending(0));
	CHECK(Status::Ok == pool.beginRenderRound(0));
	pool.serviceRetirement(16);
	CHECK_EQUAL(400, pool.freePages());
}
TEST(LoopEngine, missingRollbackStorageRejectsCaptureBeforeAnyGateOpens) {
	GestureRollback unavailable{pool};
	ResidentAudio otherAudio{pool, unavailable};
	Engine other{otherAudio, unavailable};
	auto settings = defaultSettings();
	settings.startAction.mode = ActionMode::Immediate;
	CHECK(Status::Ok == other.dispatch(Command::configure(settings), 0, clock));
	CHECK(Status::Invalid == other.dispatch({CommandType::Press, 0}, 0, clock));
	CHECK_FALSE(other.state(0).capture);
	CHECK_FALSE(other.state(0).hasPhrase);
	CHECK(other.state(0).error == Status::Invalid);
}

TEST(LoopEngine, groupStartInvalidatesShortStoppedReleaseBeforeSharedDeadline) {
	phrase();
	command(CommandType::TransportStop, 20000);
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Bar};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::Press, 20001);
	command(CommandType::StartAll, 20002);
	CHECK_EQUAL(88200, *engine.nextActionSample(clock));
	command(CommandType::Release, 30000);
	CHECK(engine.state(0).hasPhrase);
	CHECK(engine.state(0).pending);
	CHECK_EQUAL(88200, *engine.nextActionSample(clock));
	engine.service(88200, clock);
	CHECK(engine.state(0).playing);
}
TEST(LoopEngine, groupStartInvalidatesLongStoppedReleaseBeforeSharedDeadline) {
	phrase();
	command(CommandType::TransportStop, 20000);
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Bar};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::Press, 20001);
	command(CommandType::StartAll, 110001);
	CHECK_EQUAL(176400, *engine.nextActionSample(clock));
	command(CommandType::Release, 110002);
	CHECK(engine.state(0).hasPhrase);
	CHECK(engine.state(0).pending);
	CHECK_EQUAL(176400, *engine.nextActionSample(clock));
	engine.service(176400, clock);
	CHECK(engine.state(0).playing);
}
TEST(LoopEngine, groupStartInvalidatesShortStoppedReleaseAfterSharedDeadline) {
	phrase();
	command(CommandType::TransportStop, 20000);
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::Press, 20001);
	command(CommandType::StartAll, 20002);
	engine.service(22050, clock);
	samples(3);
	command(CommandType::Release, 22053);
	CHECK(engine.state(0).playing);
	CHECK_FALSE(engine.state(0).pending);
	CHECK_EQUAL(3, audio.position(0));
	CHECK_FALSE(engine.nextActionSample(clock));
}
TEST(LoopEngine, groupStartInvalidatesLongStoppedReleaseAfterSharedDeadline) {
	phrase();
	command(CommandType::TransportStop, 20000);
	auto settings = engine.settings();
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::Press, 20001);
	command(CommandType::StartAll, 20002);
	engine.service(22050, clock);
	samples(3);
	command(CommandType::Release, 108202);
	CHECK(engine.state(0).hasPhrase);
	CHECK(engine.state(0).playing);
	CHECK_FALSE(engine.state(0).pending);
	CHECK_FALSE(engine.nextActionSample(clock));
	CHECK_EQUAL(3, audio.position(0));
}

TEST(LoopEngine, configureLoopToOneShotDuringPlaybackPreservesPhaseAndStopsAtBoundary) {
	phrase();
	samples(3);
	auto settings = engine.settings();
	settings.tracks[0].playback = PlaybackMode::OneShot;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	CHECK_EQUAL(3, audio.position(0));
	CHECK(engine.state(0).playing);
	CHECK_FALSE(engine.state(0).capture);
	samples(4);
	engine.service(20004, clock);
	CHECK(engine.state(0).playing);
	CHECK_EQUAL(7, audio.position(0));
	samples(1);
	engine.service(20005, clock);
	CHECK_FALSE(engine.state(0).playing);
}
TEST(LoopEngine, configureOneShotToLoopDuringPlaybackPreservesPhaseAndKeepsWrapping) {
	phrase(true);
	samples(3);
	auto settings = engine.settings();
	settings.tracks[0].playback = PlaybackMode::Loop;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	CHECK_EQUAL(3, audio.position(0));
	CHECK(engine.state(0).playing);
	CHECK_FALSE(engine.state(0).capture);
	samples(24);
	engine.service(20024, clock);
	CHECK(engine.state(0).playing);
	CHECK_EQUAL(3, audio.position(0));
}
TEST(LoopEngine, configureLoopToOneShotDuringOverdubWrapsUntilPhasePreservingGateClose) {
	phrase(false, RecordAction::REC_DUB_PLAY);
	samples(3);
	auto settings = engine.settings();
	settings.tracks[0].playback = PlaybackMode::OneShot;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	CHECK_EQUAL(3, audio.position(0));
	CHECK(engine.state(0).capture);
	samples(16);
	engine.service(20016, clock);
	CHECK(engine.state(0).capture);
	CHECK_EQUAL(3, audio.position(0));
	command(CommandType::Press, 20016);
	CHECK_FALSE(engine.state(0).capture);
	CHECK_EQUAL(3, audio.position(0));
	samples(4);
	engine.service(20020, clock);
	CHECK(engine.state(0).playing);
	samples(1);
	engine.service(20021, clock);
	CHECK_FALSE(engine.state(0).playing);
}
TEST(LoopEngine, configureOneShotToLoopDuringOverdubWrapsAfterPhasePreservingGateClose) {
	phrase(true, RecordAction::REC_DUB_PLAY);
	samples(3);
	auto settings = engine.settings();
	settings.tracks[0].playback = PlaybackMode::Loop;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	CHECK_EQUAL(3, audio.position(0));
	CHECK(engine.state(0).capture);
	samples(16);
	engine.service(20016, clock);
	CHECK(engine.state(0).capture);
	CHECK_EQUAL(3, audio.position(0));
	command(CommandType::Press, 20016);
	CHECK_FALSE(engine.state(0).capture);
	CHECK_EQUAL(3, audio.position(0));
	samples(24);
	engine.service(20040, clock);
	CHECK(engine.state(0).playing);
	CHECK_EQUAL(3, audio.position(0));
}
TEST(LoopEngine, configureOneShotWhileExplicitStopPendingDefersCompletionUntilCancellation) {
	phrase(false, RecordAction::REC_DUB_PLAY);
	auto settings = engine.settings();
	settings.stopAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::StopAll, 20000);
	CHECK_EQUAL(22050, *engine.nextActionSample(clock));
	settings.tracks[0].playback = PlaybackMode::OneShot;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20001, clock));
	samples(24);
	engine.service(20025, clock);
	CHECK(engine.state(0).playing);
	CHECK(engine.state(0).pending);
	CHECK_EQUAL(22050, *engine.nextActionSample(clock));
	command(CommandType::Press, 20026);
	CHECK_FALSE(engine.state(0).pending);
	samples(7);
	engine.service(20033, clock);
	CHECK(engine.state(0).playing);
	samples(1);
	engine.service(20034, clock);
	CHECK_FALSE(engine.state(0).playing);
}
TEST(LoopEngine, configureLoopWhileExplicitStopPendingPreservesMusicalStopDeadline) {
	phrase(true, RecordAction::REC_DUB_PLAY);
	auto settings = engine.settings();
	settings.stopAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::StopAll, 20000);
	settings.tracks[0].playback = PlaybackMode::Loop;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20001, clock));
	samples(24);
	engine.service(20025, clock);
	CHECK(engine.state(0).playing);
	CHECK(engine.state(0).pending);
	CHECK_EQUAL(22050, *engine.nextActionSample(clock));
	engine.service(22050, clock);
	CHECK_FALSE(engine.state(0).playing);
	CHECK_FALSE(engine.state(0).pending);
}
