#include "CppUTest/TestHarness.h"
#include "deluge/loop_station/engine.h"
#include "deluge/loop_station/epochs.h"
#include "deluge/loop_station/history.h"
#include <array>
#include <vector>
using namespace loop_station;
TEST_GROUP(LoopEpochs){};
TEST(LoopEpochs, exhaustedNavigationEpochsCannotAuthorizeTargets) {
	CHECK(navigationEpochsAvailable(1, 1));
	CHECK_FALSE(navigationEpochsAvailable(kExhaustedEpoch, 1));
	CHECK_FALSE(navigationEpochsAvailable(1, kExhaustedEpoch));
	CHECK_FALSE(navigationEpochsAvailable(kExhaustedEpoch, kExhaustedEpoch));
	CHECK_EQUAL(kExhaustedEpoch, nextEpoch(kExhaustedEpoch - 1));
	CHECK_EQUAL(kExhaustedEpoch, nextEpoch(kExhaustedEpoch));
	CHECK_EQUAL(kExhaustedEpoch32, nextEpoch32(kExhaustedEpoch32 - 1));
	CHECK_EQUAL(kExhaustedEpoch32, nextEpoch32(kExhaustedEpoch32));
}
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

// Confirmed history closure of an initial take must seal it stopped, bypassing
// the configured REC_DUB_PLAY automatic Play/Dub and quantized stop deadlines.
TEST(LoopEngine, historyClosureOfInitialTakeNeverEntersAutomaticDub) {
	auto settings = engine.settings();
	settings.recordAction = RecordAction::REC_DUB_PLAY;
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	settings.stopAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 0, clock));
	command(CommandType::Press, 0);
	CHECK(engine.state(0).pending);
	engine.service(22050, clock);
	CHECK(engine.state(0).capture);
	CHECK_FALSE(engine.state(0).playing);
	samples(8);
	command(CommandType::Press, 22058);
	CHECK(engine.state(0).pending);
	CHECK(Status::Ok == engine.closeCaptureForHistory(0));
	CHECK_FALSE(engine.state(0).capture);
	CHECK_FALSE(engine.state(0).playing);
	CHECK(engine.state(0).hasPhrase);
	CHECK_EQUAL(8, engine.state(0).phraseFrames);
	CHECK_FALSE(engine.state(0).pending);
	CHECK_FALSE(engine.nextActionSample(clock));
	// The bypassed quantized deadline must not later begin playback or dubbing.
	engine.service(44100, clock);
	CHECK_FALSE(engine.state(0).capture);
	CHECK_FALSE(engine.state(0).playing);
	// A zero-frame take closes to empty without manufacturing a phrase or Redo.
	auto fresh = defaultSettings();
	fresh.startAction.mode = ActionMode::Immediate;
	fresh.stopAction.mode = ActionMode::Immediate;
	CHECK(Status::Ok == engine.dispatch(Command::configure(fresh), 44101, clock));
	command(CommandType::Press, 44101, 1);
	CHECK(engine.state(1).capture);
	CHECK(Status::Ok == engine.closeCaptureForHistory(1));
	CHECK_FALSE(engine.state(1).hasPhrase);
	CHECK_FALSE(engine.state(1).capture);
	CHECK_FALSE(engine.state(1).playing);
	CHECK_EQUAL(0, engine.state(1).phraseFrames);
}

// Confirmed history closure of an overdub confirms provisional additions
// instead of cancelling them, keeps playback, and bypasses quantized settings.
TEST(LoopEngine, historyClosureOfOverdubKeepsPlaybackAndRecordedFrames) {
	phrase(false, RecordAction::REC_PLAY_DUB, 0);
	CHECK_FALSE(engine.state(0).capture);
	samples(1);
	auto settings = engine.settings();
	settings.recordAction = RecordAction::REC_DUB_PLAY;
	settings.startAction = {ActionMode::Synced, Interval::Quarter};
	settings.stopAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 1000, clock));
	command(CommandType::Press, 1000);
	CHECK(engine.state(0).capture);
	CHECK(engine.state(0).playing);
	CHECK(rollback.provisional(0));
	samples(3, 0, {100, -100});
	CHECK(Status::Ok == engine.closeCaptureForHistory(0));
	CHECK_FALSE(engine.state(0).capture);
	CHECK(engine.state(0).playing);
	CHECK_FALSE(engine.state(0).pending);
	CHECK_FALSE(rollback.provisional(0));
	CHECK_FALSE(engine.nextActionSample(clock));
	const auto retained = pool.sealedHandle(0);
	CHECK(retained.valid());
	// Frame 1 was overdubbed with +100; a cancel would have restored the original 10.
	CHECK_EQUAL(110, pool.frame(retained, 1)->l);
	// The cleared tap means the next press opens a fresh overdub, not a double-tap stop.
	command(CommandType::Press, 2000);
	CHECK(engine.state(0).capture);
	CHECK(engine.state(0).playing);
	CHECK(Status::Ok == engine.closeCaptureForHistory(0));
	CHECK_FALSE(engine.state(0).capture);
}

// A previously playing recording is published empty atomically in one boundary
// as pool.detach -> resident.detachForHistory -> engine.phraseRemoved, with the
// retained generation/samples exact and the track's selection/settings kept.
TEST(LoopEngine, phraseRemovalPreservesSelectionAndSettings) {
	phrase(false, RecordAction::REC_PLAY_DUB, 0);
	CHECK(engine.state(0).playing);
	auto settings = engine.settings();
	settings.tracks[0].source = Source::Balanced;
	settings.tracks[0].monitoring = Monitoring::Off;
	settings.tracks[0].playback = PlaybackMode::OneShot;
	settings.tracks[0].colour = Colour::Magenta;
	settings.tracks[0].transposeCents = 700;
	CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 20000, clock));
	command(CommandType::Select, 20000, 0);
	CHECK_EQUAL(0, engine.selected());
	const auto active = pool.sealedHandle(0);
	CHECK(active.valid());
	const auto epoch = engine.historyEpoch(0);
	const auto stream = engine.streamGeneration(0);
	const auto free = pool.freePages();
	PhraseHandle retained;
	CHECK(Status::Ok == pool.detach(0, retained));
	CHECK_EQUAL(active.generation, retained.generation);
	CHECK(Status::Ok == audio.detachForHistory(0));
	engine.phraseRemoved(0);
	const auto state = engine.state(0);
	CHECK_FALSE(state.hasPhrase);
	CHECK_FALSE(state.playing);
	CHECK_FALSE(state.capture);
	CHECK_EQUAL(0, state.phraseFrames);
	CHECK_EQUAL(0, engine.selected());
	const auto& kept = engine.settings();
	CHECK(kept.tracks[0].source == Source::Balanced);
	CHECK(kept.tracks[0].monitoring == Monitoring::Off);
	CHECK(kept.tracks[0].playback == PlaybackMode::OneShot);
	CHECK(kept.tracks[0].colour == Colour::Magenta);
	CHECK_EQUAL(700, kept.tracks[0].transposeCents);
	CHECK(engine.historyEpoch(0) != epoch);
	CHECK(engine.streamGeneration(0) != stream);
	CHECK_FALSE(engine.nextActionSample(clock));
	// No orphan live provider: the emptied slot renders silence.
	CHECK_FALSE(audio.playing(0));
	const Frame input[] = {{5, -5}};
	Frame output[] = {{99, 99}};
	audio.render(0, input, output);
	CHECK_EQUAL(0, output[0].l);
	CHECK_EQUAL(0, output[0].r);
	// Retained exact samples and generation for Redo; pages were not freed.
	CHECK_EQUAL(free, pool.freePages());
	CHECK_EQUAL(8, pool.length(retained));
	CHECK_EQUAL(10, pool.frame(retained, 0)->l);
	CHECK_EQUAL(-20, pool.frame(retained, 7)->r);
}

// An obsolete pad release after removal must neither clear nor restart the
// removed phrase; a later fresh press begins a new recording.
TEST(LoopEngine, oldPadReleaseCannotClearOrRestartRemovedPhrase) {
	phrase(false, RecordAction::REC_PLAY_DUB, 0);
	command(CommandType::TransportStop, 20000);
	command(CommandType::Press, 20001);
	CHECK_EQUAL(0, engine.heldDurationSamples(0, 20001));
	PhraseHandle retained;
	CHECK(Status::Ok == pool.detach(0, retained));
	CHECK(Status::Ok == audio.detachForHistory(0));
	engine.phraseRemoved(0);
	CHECK_FALSE(engine.state(0).hasPhrase);
	command(CommandType::Release, 20001 + 3 * kSampleRate);
	CHECK_FALSE(engine.state(0).hasPhrase);
	CHECK_FALSE(engine.state(0).playing);
	CHECK_FALSE(engine.state(0).pending);
	CHECK_EQUAL(0, engine.heldDurationSamples(0, 20001 + 3 * kSampleRate));
	command(CommandType::Press, 400000);
	CHECK(engine.state(0).capture);
	CHECK_FALSE(engine.state(0).playing);
}

// History identity advances for real session transitions, never for incoming
// frames or for provisional expiry inside a single capture session.
TEST(LoopEngine, historyEpochTracksSessionsNotSamples) {
	phrase(false, RecordAction::REC_PLAY_DUB, 0);
	const auto base = engine.historyEpoch(0);
	samples(200);
	CHECK_EQUAL(base, engine.historyEpoch(0));
	command(CommandType::Press, 1000);
	const auto capturing = engine.historyEpoch(0);
	CHECK(capturing != base);
	// Provisional expiry driven by the engine's authoritative clock must not
	// change the capture identity.
	for (SampleClock now = 1000; now <= 1000 + GestureRollback::kProvisionalSamples; ++now)
		engine.service(now, clock);
	CHECK_FALSE(rollback.provisional(0));
	CHECK_EQUAL(capturing, engine.historyEpoch(0));
	samples(50, 0, {1, 1});
	CHECK_EQUAL(capturing, engine.historyEpoch(0));
	CHECK(Status::Ok == engine.closeCaptureForHistory(0));
	const auto closed = engine.historyEpoch(0);
	CHECK(closed != capturing);
	command(CommandType::TransportStop, 20000);
	CHECK_FALSE(engine.state(0).playing);
	const auto stopped = engine.historyEpoch(0);
	CHECK(stopped != closed);
	engine.phraseReplaced(0, 5, true);
	CHECK(engine.historyEpoch(0) != stopped);
}

namespace {
struct CaptureAdmissionFixture {
	static constexpr FrameIndex perPage = kPageBytes / sizeof(Frame);
	std::vector<std::byte> backing, scratch, historyStorage;
	PagePool pool;
	GestureRollback rollback{pool};
	History history{pool, rollback};
	ResidentAudio audio{pool, rollback};
	Engine engine{audio, rollback};
	ClockMap clock;
	void setup(unsigned pages, unsigned historyPages) {
		backing.resize(pages * kPageBytes);
		scratch.resize(GestureRollback::requiredBytes());
		historyStorage.resize(historyPages * kPageBytes);
		CHECK(Status::Ok == pool.initialize(backing));
		CHECK(Status::Ok == rollback.initialize(scratch));
		CHECK(Status::Ok == history.initialize(historyStorage));
		audio.setWriteObservers(&history, nullptr);
		auto settings = defaultSettings();
		settings.startAction.mode = settings.stopAction.mode = ActionMode::Immediate;
		CHECK(Status::Ok == engine.dispatch(Command::configure(settings), 0, clock));
	}
	PhraseHandle makePhrase(SlotId slot, FrameIndex frames) {
		std::vector<Frame> input(frames), output(frames);
		for (FrameIndex i = 0; i < frames; ++i)
			input[i] = {int32_t(i + 1), -int32_t(i + 1)};
		CHECK(Status::Ok == audio.beginInitial(slot, frames));
		audio.render(slot, input, output);
		PhraseHandle handle;
		CHECK(Status::Ok == audio.finishInitial(slot, handle));
		CHECK(Status::Ok == audio.start(slot, false));
		return handle;
	}
	PhraseHandle makeRetainedInitial(SlotId slot, FrameIndex frames) {
		const auto handle = makePhrase(slot, frames);
		RestoreId id{};
		CHECK(Status::Ok == history.requestRestore(slot, false, id));
		Status reserved = Status::Busy;
		for (unsigned attempt = 0; attempt < 64 && reserved == Status::Busy; ++attempt)
			reserved = history.reserveRestore(id, 8);
		CHECK_EQUAL(int(Status::Ok), int(reserved));
		Status serviced = Status::Busy;
		while (serviced == Status::Busy)
			serviced = history.serviceRestore(id, 8);
		CHECK_EQUAL(int(Status::Ok), int(serviced));
		CHECK(Status::Ok == history.commitRestore(id));
		CHECK(Status::Ok == audio.detachForHistory(slot));
		history.mappingReplaced(slot);
		return handle;
	}
	void overdubLayer(SlotId slot, Frame value = {3, -3}) {
		CHECK(Status::Ok == audio.setCaptureGate(slot, true));
		Frame output{};
		audio.render(slot, {&value, 1}, {&output, 1});
		CHECK(Status::Ok == audio.setCaptureGate(slot, false));
	}
	void undoOrdinary(SlotId slot) {
		RestoreId id{};
		CHECK(Status::Ok == history.requestRestore(slot, false, id));
		Status reserved = Status::Busy;
		for (unsigned attempt = 0; attempt < 64 && reserved == Status::Busy; ++attempt)
			reserved = history.reserveRestore(id, 8);
		CHECK_EQUAL(int(Status::Ok), int(reserved));
		Status serviced = Status::Busy;
		while (serviced == Status::Busy)
			serviced = history.serviceRestore(id, 8);
		CHECK_EQUAL(int(Status::Ok), int(serviced));
		CHECK(Status::Ok == history.commitRestore(id));
		const auto current = pool.sealedHandle(slot);
		CHECK(current.valid());
		CHECK(Status::Ok == audio.replace(slot, current));
		history.mappingReplaced(slot);
	}
	void press(SlotId slot, SampleClock now) {
		CHECK(Status::Ok == engine.dispatch({CommandType::Press, slot}, now, clock));
	}
};
} // namespace

TEST_GROUP(LoopCaptureAdmission){};

// I4: a fixed-duration take must retain accepted pad intent across bounded
// foreground reclamation instead of failing immediately or needing a new press.
TEST(LoopCaptureAdmission, fixedDurationInitialWaitsForBoundedForegroundReclamation) {
	CaptureAdmissionFixture f;
	f.setup(200, 64);
	f.makeRetainedInitial(1, 180 * CaptureAdmissionFixture::perPage);
	CHECK_EQUAL(20, f.pool.freePages());
	Command duration{CommandType::SetDuration};
	duration.slot = 0;
	duration.durationBars = 1;
	CHECK(Status::Ok == f.engine.dispatch(duration, 0, f.clock));
	const auto needed = f.clock.samplesForBars(1);
	CHECK(needed > 20 * CaptureAdmissionFixture::perPage);
	f.press(0, 100);
	auto state = f.engine.state(0);
	CHECK(state.pending);
	CHECK(PendingAction::Initial == state.action);
	CHECK_FALSE(state.capture);
	CHECK_FALSE(state.hasPhrase);
	CHECK_EQUAL(int(Status::Ok), int(state.error)); // Accepted, not refused.
	unsigned quanta = 0;
	while (!f.engine.state(0).capture && quanta < 32) {
		f.engine.serviceCaptureAdmission(100, f.clock);
		++quanta;
	}
	CHECK(quanta > 1); // Reclamation genuinely spanned bounded foreground quanta.
	state = f.engine.state(0);
	CHECK(state.capture);
	CHECK_FALSE(state.pending);
	CHECK_FALSE(state.playing);
	CHECK_FALSE(state.hasPhrase); // Nothing published before admission completes.
	CHECK_EQUAL(int(Status::Ok), int(state.error));
}
// I4: a manual take must not open a zero-page capture that capacity-completes
// immediately while optional history owns every live page.
TEST(LoopCaptureAdmission, manualInitialAdmitsARealPageInsteadOfZeroPageCompletion) {
	CaptureAdmissionFixture f;
	f.setup(4, 8);
	f.makeRetainedInitial(1, 4 * CaptureAdmissionFixture::perPage);
	CHECK_EQUAL(0, f.pool.freePages());
	f.press(0, 100);
	auto state = f.engine.state(0);
	CHECK_EQUAL(int(Status::Ok), int(state.error)); // Accepted, never refused for optional history.
	for (unsigned i = 0; i < 8 && !f.engine.state(0).capture; ++i)
		f.engine.serviceCaptureAdmission(100, f.clock);
	state = f.engine.state(0);
	CHECK(state.capture);
	CHECK_FALSE(state.playing);
	CHECK_FALSE(state.hasPhrase);
	CHECK(Status::Ok == f.pool.beginRenderRound(f.audio.manualParticipants()));
	const auto capacity = f.audio.initialCapacity(0);
	CHECK(capacity.has_value());
	CHECK_EQUAL(int(CaptureAdmissionFixture::perPage), int(*capacity));
	const Frame input{9, -9};
	Frame output{};
	f.audio.render(0, {&input, 1}, {&output, 1});
	CHECK_EQUAL(1, f.audio.recordedInitialFrames(0));
	CHECK(f.engine.state(0).capture); // No capacity completion from the admitted page.
}
// I4: if no optional history can free the pages, protected exhaustion remains an
// honest failure and the live phrase is preserved.
TEST(LoopCaptureAdmission, protectedExhaustionFailsHonestlyAndPreservesLivePhrase) {
	CaptureAdmissionFixture f;
	f.setup(4, 8);
	const auto live = f.makePhrase(1, 4 * CaptureAdmissionFixture::perPage);
	CHECK_EQUAL(0, f.pool.freePages());
	f.press(0, 100);
	for (unsigned i = 0; i < 8 && f.engine.state(0).error == Status::Ok; ++i)
		f.engine.serviceCaptureAdmission(100, f.clock);
	const auto state = f.engine.state(0);
	CHECK_FALSE(state.capture);
	CHECK_FALSE(state.pending);
	CHECK_EQUAL(int(Status::NoMemory), int(state.error));
	CHECK_EQUAL(4 * CaptureAdmissionFixture::perPage, f.pool.length(live));
	CHECK_EQUAL(1, f.pool.frame(live, 0)->l);
	CHECK(f.audio.playing(1));
}
// I4: disabled history owns nothing, so admission must not defer forever waiting
// for a reclamation source that does not exist.
TEST(LoopCaptureAdmission, disabledHistoryReportsHonestExhaustion) {
	CaptureAdmissionFixture f;
	f.setup(2, 0);
	f.makePhrase(1, 2 * CaptureAdmissionFixture::perPage);
	CHECK_EQUAL(0, f.pool.freePages());
	f.press(0, 100);
	for (unsigned i = 0; i < 4 && f.engine.state(0).error == Status::Ok; ++i)
		f.engine.serviceCaptureAdmission(100, f.clock);
	const auto state = f.engine.state(0);
	CHECK_FALSE(state.capture);
	CHECK_FALSE(state.pending);
	CHECK_EQUAL(int(Status::NoMemory), int(state.error));
}

// N1: when all 24 PagePool records are occupied partly by optional retained
// history, an empty target must defer and reclaim a record in the foreground
// instead of being classified as structurally impossible.
TEST(LoopCaptureAdmission, temporaryRecordExhaustionReclaimsOptionalHistory) {
	CaptureAdmissionFixture f;
	f.setup(32, 64);
	f.makeRetainedInitial(0, CaptureAdmissionFixture::perPage);
	for (SlotId slot = 1; slot < kTrackCount; ++slot)
		f.makePhrase(slot, CaptureAdmissionFixture::perPage);
	for (SlotId slot = 1; slot < 6; ++slot) {
		for (unsigned layer = 0; layer < 3; ++layer)
			f.overdubLayer(slot);
		for (unsigned undo = 0; undo < 3; ++undo)
			f.undoOrdinary(slot);
	}
	f.overdubLayer(6);
	f.undoOrdinary(6);
	CHECK_FALSE(f.pool.hasCaptureRecord(0));
	CHECK_FALSE(f.pool.canBeginCapture(0, 0));
	// The accepted press is preserved, not rejected before foreground reclamation.
	CHECK(Status::Ok == f.engine.dispatch({CommandType::Press, 0}, 100, f.clock));
	auto state = f.engine.state(0);
	CHECK(state.pending);
	CHECK(PendingAction::Initial == state.action);
	CHECK_FALSE(state.capture);
	CHECK_EQUAL(int(Status::Ok), int(state.error));
	for (unsigned i = 0; i < 32 && !f.engine.state(0).capture; ++i)
		f.engine.serviceCaptureAdmission(100, f.clock);
	state = f.engine.state(0);
	CHECK(state.capture);
	CHECK_FALSE(state.pending);
	CHECK_EQUAL(int(Status::Ok), int(state.error));
}
// N2: admission builds bounded page headroom for a manual take, and the
// foreground cushion keeps an already-admitted capture running across pages.
TEST(LoopCaptureAdmission, manualCaptureMaintainsHeadroomAcrossMultiplePages) {
	CaptureAdmissionFixture f;
	f.setup(5, 16);
	f.makeRetainedInitial(1, 2 * CaptureAdmissionFixture::perPage);
	f.makeRetainedInitial(2, 2 * CaptureAdmissionFixture::perPage);
	CHECK_EQUAL(1, f.pool.freePages());
	Status admitted = Status::Busy;
	for (unsigned i = 0; i < 8 && admitted == Status::Busy; ++i)
		admitted = f.history.reclaimForCapture(0, 0);
	CHECK_EQUAL(int(Status::Ok), int(admitted));
	CHECK(f.pool.freePages() >= kManualCaptureHeadroomPages);
	CHECK(Status::Ok == f.audio.beginInitial(0, 0));
	auto renderPage = [&] {
		CHECK(Status::Ok == f.pool.beginRenderRound(f.audio.manualParticipants()));
		std::vector<Frame> input(CaptureAdmissionFixture::perPage, Frame{1, -1});
		std::vector<Frame> output(CaptureAdmissionFixture::perPage);
		f.audio.render(0, input, output);
	};
	renderPage();
	renderPage();
	CHECK_EQUAL(2 * CaptureAdmissionFixture::perPage, f.audio.recordedInitialFrames(0));
	CHECK(f.pool.freePages() < kManualCaptureHeadroomPages);
	Status headroom = Status::Busy;
	for (unsigned i = 0; i < 8 && headroom == Status::Busy; ++i)
		headroom = f.history.ensureCaptureHeadroom(0, kManualCaptureHeadroomPages);
	CHECK_EQUAL(int(Status::Ok), int(headroom));
	CHECK(f.pool.freePages() >= kManualCaptureHeadroomPages);
	renderPage();
	CHECK_EQUAL(3 * CaptureAdmissionFixture::perPage, f.audio.recordedInitialFrames(0));
	CHECK(Status::Ok == f.audio.setCaptureGate(0, false));
}
// N3: a deferred Initial must not hide another slot's future quantized Start in
// the audio adapter's exact sample-boundary selection.
TEST(LoopCaptureAdmission, deferredAdmissionDoesNotMaskAnotherSlotFutureDeadline) {
	CaptureAdmissionFixture f;
	f.setup(200, 64);
	f.makeRetainedInitial(1, 195 * CaptureAdmissionFixture::perPage);
	CHECK_EQUAL(5, f.pool.freePages());
	// A stopped phrase on slot 2 supplies the future quantized Start.
	CHECK(Status::Ok == f.engine.dispatch({CommandType::Press, 2}, 100, f.clock));
	CHECK(f.engine.state(2).capture);
	CHECK(Status::Ok == f.pool.beginRenderRound(f.audio.manualParticipants()));
	Frame input{1, -1}, output{};
	f.audio.render(2, {&input, 1}, {&output, 1});
	CHECK(Status::Ok == f.engine.dispatch({CommandType::Press, 2}, 100, f.clock));
	CHECK(f.engine.state(2).playing);
	CHECK(Status::Ok == f.engine.dispatch({CommandType::TransportStop}, 200, f.clock));
	CHECK_FALSE(f.engine.state(2).playing);
	auto synced = f.engine.settings();
	synced.startAction = {ActionMode::Synced, Interval::Quarter};
	CHECK(Status::Ok == f.engine.dispatch(Command::configure(synced), 200, f.clock));
	CHECK(Status::Ok == f.engine.dispatch({CommandType::StartAll}, 200, f.clock));
	const auto future = f.engine.nextActionSample(f.clock);
	CHECK(future.has_value());
	CHECK(*future > 200);
	auto immediate = f.engine.settings();
	immediate.startAction.mode = ActionMode::Immediate;
	CHECK(Status::Ok == f.engine.dispatch(Command::configure(immediate), 200, f.clock));
	Command duration{CommandType::SetDuration};
	duration.slot = 0;
	duration.durationBars = 1;
	CHECK(Status::Ok == f.engine.dispatch(duration, 200, f.clock));
	CHECK(Status::Ok == f.engine.dispatch({CommandType::Press, 0}, 200, f.clock));
	const auto state = f.engine.state(0);
	CHECK(state.pending);
	CHECK(PendingAction::Initial == state.action);
	CHECK_FALSE(state.capture);
	CHECK_EQUAL(int(*future), int(*f.engine.nextActionSample(f.clock)));
}
