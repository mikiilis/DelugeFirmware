#!/usr/bin/env python3
"""Extract unchanged physical Buttons and SettingsMenu dispatch bodies."""

import argparse
from pathlib import Path


def section(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1:
        raise SystemExit(f"missing or ambiguous production marker: {begin!r}")
    start = source.index(begin)
    finish = source.find(end, start + len(begin))
    if finish < 0:
        raise SystemExit(f"missing production end marker: {end!r}")
    return source[start:finish]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--buttons", type=Path, required=True)
    parser.add_argument("--view", type=Path, required=True)
    parser.add_argument("--settings", type=Path, required=True)
    parser.add_argument("--support", type=Path, required=True)
    parser.add_argument("--tests", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    buttons = args.buttons.read_text()
    button_body = section(
        buttons, "ActionResult buttonAction(", "void commandToggleShift("
    )
    encoder_dispatch = section(
        buttons, "void dispatchModEncoderButtonAction(", "ActionResult buttonAction("
    )
    view = section(
        args.view.read_text(),
        "bool View::consumePendingStutterEncoderRelease(",
        "void View::setActiveModControllableWithoutTimelineCounter(",
    )
    settings = section(
        args.settings.read_text(),
        "ActionResult SettingsMenu::buttonAction(",
        "ActionResult SettingsMenu::padAction(",
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        f'#include "{args.support.as_posix()}"\n'
        "ProbeDisplay* display = nullptr;\nUI* currentUI = nullptr;\nUI* rootUI = nullptr;\n"
        "View view{};\nTimerManager uiTimerManager{};\nRuntimeSettings runtimeFeatureSettings{};\n"
        "AudioRecorder audioRecorder{};\nPlaybackHandler playbackHandler{};\nPlaybackMode playbackMode{};\n"
        "PlaybackMode* currentPlaybackMode = &playbackMode;\nLoadUI loadSongUI{};\n"
        "UI sessionView{}, arrangerView{};\nuint32_t AudioEngine::audioSampleTimer = 0;\n"
        "bool sdRoutineLock = false;\n"
        + view
        + "namespace Buttons {\n"
        + "bool recordButtonPressUsedUp = false, selectButtonPressUsedUp = false;\n"
        "bool shiftCurrentlyPressed = false, considerShiftReleaseForSticky = false;\n"
        "bool considerCrossScreenReleaseForCrossScreenMode = false;\n"
        "bool buttonStates[NUM_BUTTON_COLS + 1][NUM_BUTTON_ROWS]{};\n"
        "uint32_t timeRecordButtonPressed = 0;\n"
        "void commandToggleShift(bool) {}\n"
        + encoder_dispatch
        + button_body
        + "}\n"
        + "bool shouldCloseSettingsMenu(bool, bool, bool) { return false; }\n"
        + "using namespace deluge::hid::button;\n"
        + settings
        + args.tests.read_text()
    )


if __name__ == "__main__":
    main()
