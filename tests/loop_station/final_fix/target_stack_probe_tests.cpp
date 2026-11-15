namespace {
void check(bool condition, const char* message) {
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		std::abort();
	}
}
} // namespace
int main() {
	Song song;
	currentSong = &song;
	view.setActiveModControllableTimelineCounter(&song, false, false);
	check(!song.affectEntire, "baseline affectEntire remains false");
	check(!view.activeModControllableModelStack.timelineCounter, "ordinary Song policy clears timeline");
	check(!view.activeModControllableModelStack.modControllable, "ordinary Song policy clears effectable");
	check(!view.activeModControllableModelStack.paramManager, "ordinary Song policy clears ParamManager");

	song.affectEntire = true;
	view.setActiveModControllableTimelineCounter(&song, false, false);
	check(view.activeModControllableModelStack.timelineCounter == &song, "affectEntire Song timeline");
	check(view.activeModControllableModelStack.modControllable == &song.globalEffectable,
	      "affectEntire Song effectable");
	check(view.activeModControllableModelStack.paramManager == &song.paramManager, "affectEntire Song ParamManager");

	song.affectEntire = false;
	view.setActiveModControllableTimelineCounter(&song, false, true);
	check(!song.affectEntire, "explicit fallback does not mutate persisted affectEntire");
	check(view.activeModControllableModelStack.timelineCounter == &song, "forced fallback Song timeline");
	check(view.activeModControllableModelStack.modControllable == &song.globalEffectable,
	      "forced fallback Song effectable");
	check(view.activeModControllableModelStack.paramManager == &song.paramManager, "forced fallback Song ParamManager");

	AudioClip clip;
	view.setActiveModControllableTimelineCounter(&clip, false, false);
	check(view.activeModControllableModelStack.timelineCounter == &clip, "selected clip timeline");
	check(view.activeModControllableModelStack.modControllable == &clip.mod, "selected clip effectable");
	check(view.activeModControllableModelStack.paramManager == &clip.params, "selected clip ParamManager");

	SongModel loopModel;
	loopModel.boundClip = &clip;
	song.model = &loopModel;
	song.affectEntire = false;
	loopModel.engine_.selectedSlot = kInvalidSlot;
	LoopStationView loopView;
	view.setActiveModControllableTimelineCounter(&clip, false, false);
	loopView.updateModTarget();
	check(!loopView.modTargetIsClip_, "no-selection context is Song");
	check(view.activeModControllableModelStack.timelineCounter == &song, "no-selection target timeline");
	check(view.activeModControllableModelStack.modControllable == &song.globalEffectable,
	      "no-selection target effectable with flag off");
	check(view.activeModControllableModelStack.paramManager == &song.paramManager,
	      "no-selection target ParamManager with flag off");
	check(!song.affectEntire, "no-selection fallback preserves flag");

	loopModel.engine_.selectedSlot = 2;
	loopModel.boundClip = &clip;
	loopView.updateModTarget();
	check(loopView.modTargetIsClip_, "bound selected clip context");
	check(song.currentClip == &clip, "selected clip synchronizes Song currentClip");
	check(view.activeModControllableModelStack.timelineCounter == &clip, "selected clip target timeline");
	check(view.activeModControllableModelStack.modControllable == &clip.mod, "selected clip target effectable");
	check(view.activeModControllableModelStack.paramManager == &clip.params, "selected clip target ParamManager");

	loopModel.boundClip = nullptr;
	loopView.updateModTarget();
	check(!loopView.modTargetIsClip_, "unbound selected slot falls back to Song context");
	check(view.activeModControllableModelStack.timelineCounter == &song, "unbound fallback timeline");
	check(view.activeModControllableModelStack.modControllable == &song.globalEffectable,
	      "unbound fallback effectable");
	check(view.activeModControllableModelStack.paramManager == &song.paramManager, "unbound fallback ParamManager");
	loopModel.boundClip = &clip;
	loopModel.engine_.selectedSlot = 2;

	int32_t clipStutterRate = 0;
	constexpr int32_t oldClipRate = 19 << 24;
	int clipManager = 7;
	ModControllableAudio clipOwner;
	clipOwner.rate = &clipStutterRate;
	clipOwner.savedRate = oldClipRate;
	view.activeModControllableModelStack.modControllable = &clipOwner;
	view.activeModControllableModelStack.paramManager = &clipManager;
	currentUIMode = UI_MODE_STUTTERING;
	destinationSetupSequence = sourceRestoreSequence = probeSequence = 0;
	loopView.attemptExit();
	check(clipStutterRate == oldClipRate, "Session exit restores clip source rate");
	check(clipOwner.receivedParamManager == &clipManager, "Session exit cleanup uses clip source manager");
	song.affectEntire = true;
	sessionView.focusRegained();
	check(sourceRestoreSequence < destinationSetupSequence,
	      "clip stutter restored before actual exit and Session focus setup");
	check(view.activeModControllableModelStack.timelineCounter == &song, "real Session focus installs Song target");
	check(view.activeModControllableModelStack.modControllable == &song.globalEffectable,
	      "real Session focus installs Song effectable");
	check(view.activeModControllableModelStack.paramManager == &song.paramManager,
	      "real Session focus installs Song ParamManager");
	check(view.pendingStutterEncoderRelease_, "stutter lifecycle arms held release ownership");

	int32_t songStutterRate = 0;
	constexpr int32_t oldSongRate = -(21 << 24);
	int songManager = 8;
	ModControllableAudio songOwner;
	songOwner.rate = &songStutterRate;
	songOwner.savedRate = oldSongRate;
	view.activeModControllableModelStack.modControllable = &songOwner;
	view.activeModControllableModelStack.paramManager = &songManager;
	currentUIMode = UI_MODE_STUTTERING;
	check(isUIModeActive(UI_MODE_STUTTERING), "stutter mode is active before real Clip branch");
	check(view.activeModControllableModelStack.modControllable == &songOwner,
	      "outgoing Song owner is active before branch");
	check(model() == &loopModel, "Loop Station model exists before Clip branch");
	check(model()->engine().selected() == 2, "selected slot exists before Clip branch");
	check(model()->clip(2) == &clip, "selected slot has bound clip before Clip branch");
	destinationSetupSequence = sourceRestoreSequence = probeSequence = 0;
	check(loopView.clipViewNavigation(deluge::hid::button::CLIP_VIEW, true, true) == ActionResult::DEALT_WITH,
	      "actual Loop Station Clip View branch handles navigation");
	check(song.currentClip == &clip, "production clip transition installs selected Song clip");
	check(currentRoot == &audioClipView, "production no-sample transition changes root to Audio Clip UI");
	check(songOwner.receivedParamManager == &songManager, "Clip View branch dispatches original-owner cleanup");
	check(songStutterRate == oldSongRate, "Clip View navigation restores Song source rate");
	check(songOwner.receivedParamManager == &songManager, "Clip View cleanup uses Song source manager");
	audioClipView.focusRegained();
	check(sourceRestoreSequence < destinationSetupSequence,
	      "Song stutter restored before actual Clip branch and focus setup");
	check(view.activeModControllableModelStack.timelineCounter == &clip,
	      "real Clip focus installs selected Clip target");
	check(view.activeModControllableModelStack.modControllable == &clip.mod,
	      "real Clip focus installs Clip effectable");
	check(view.activeModControllableModelStack.paramManager == &clip.params,
	      "real Clip focus installs Clip ParamManager");
	check(clip.mod == 5 && clip.params == 6, "destination Clip parameters remain unchanged");
	return 0;
}
