#!/usr/bin/env python3
"""Compile unchanged Song and View target-stack production methods in isolation."""

import argparse
from pathlib import Path


def extract(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1:
        raise SystemExit(f"missing or ambiguous production marker: {begin!r}")
    start = source.index(begin)
    finish = source.find(end, start + len(begin))
    if finish < 0:
        raise SystemExit(f"missing production end marker: {end!r}")
    return source[start:finish]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--view", type=Path, required=True)
    parser.add_argument("--song", type=Path, required=True)
    parser.add_argument("--loop-view", type=Path, required=True)
    parser.add_argument("--session-view", type=Path, required=True)
    parser.add_argument("--audio-view", type=Path, required=True)
    parser.add_argument("--support", type=Path, required=True)
    parser.add_argument("--tests", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    view = extract(
        args.view.read_text(),
        "void View::setActiveModControllableTimelineCounter(",
        "bool View::finishActiveStutterForModTargetChange(",
    )
    stutter_lifecycle = extract(
        args.view.read_text(),
        "bool View::finishActiveStutterForModTargetChange()",
        "bool View::consumePendingStutterEncoderRelease(",
    )
    song = args.song.read_text()
    song_methods = extract(
        song, "void Song::getActiveModControllable(", "void Song::expectEvent()"
    )
    song_setup = extract(
        song,
        "ModelStackWithThreeMainThings* Song::setupModelStackWithSongAsTimelineCounter(",
        "ModelStackWithTimelineCounter* Song::setupModelStackWithCurrentClip(",
    )
    loop_target = extract(
        args.loop_view.read_text(),
        "void LoopStationView::updateModTarget()",
        "void LoopStationView::finishStutterBeforeTargetChange()",
    )
    outgoing_cleanup = extract(
        args.loop_view.read_text(),
        "void LoopStationView::finishStutterBeforeTargetChange()",
        "void LoopStationView::updateLed()",
    )
    attempt_exit = extract(
        args.loop_view.read_text(),
        "void LoopStationView::attemptExit()",
        "void LoopStationView::graphicsRoutine()",
    )
    clip_branch = extract(
        args.loop_view.read_text(), "\t\tif (b == CLIP_VIEW)", "\t\tif (b == SELECT_ENC"
    )
    session_focus = extract(
        args.session_view.read_text(),
        "void SessionView::focusRegained()",
        "ActionResult SessionView::buttonAction(",
    )
    audio_focus = extract(
        args.audio_view.read_text(),
        "void AudioClipView::focusRegained()",
        "void AudioClipView::renderOLED(",
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        f'#include "{args.support.as_posix()}"\n'
        "Song* currentSong = nullptr;\nbool renderedVUMeter = false;\nView view{};\n"
        "DisplayProbe* display = nullptr;\nSessionView sessionView{};\nAudioClipView audioClipView{};\n"
        + song_methods
        + song_setup
        + view
        + stutter_lifecycle
        + loop_target
        + outgoing_cleanup
        + attempt_exit
        + session_focus
        + audio_focus
        + "ActionResult LoopStationView::clipViewNavigation(deluge::hid::Button b, bool on, bool loopOwnsInput) {\n"
        + "auto* m = model();\nif (loopOwnsInput) {\nusing namespace deluge::hid::button;\n"
        + clip_branch
        + "}\nreturn ActionResult::NOT_DEALT_WITH;\n}\n"
        + args.tests.read_text()
    )


if __name__ == "__main__":
    main()
