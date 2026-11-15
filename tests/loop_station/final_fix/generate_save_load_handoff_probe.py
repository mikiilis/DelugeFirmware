#!/usr/bin/env python3
"""Extract the unchanged Save/Load branches and Loop Station delegation tail."""

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
    parser.add_argument("--view", type=Path, required=True)
    parser.add_argument("--loop-view", type=Path, required=True)
    parser.add_argument("--support", type=Path, required=True)
    parser.add_argument("--tests", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    shared = section(
        args.view.read_text(), "\t// Save button", "\t// Sync-scaling button"
    )
    root_handler = section(
        args.loop_view.read_text(),
        "ActionResult LoopStationView::buttonAction(",
        "bool LoopStationView::renderMainPads(",
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        f'#include "{args.support.as_posix()}"\n'
        "ProbeDisplay* display = nullptr;\nUI* currentUI = nullptr;\nUI* rootUI = nullptr;\n"
        "Song* currentSong = nullptr;\nView view{};\nSessionView sessionView{};\n"
        "ActionResult sharedSaveLoadBranch(deluge::hid::Button b, bool on, bool inCardRoutine) {\n"
        "using namespace deluge::hid::button;\nif (false) {}\n"
        + shared
        + "\nreturn ActionResult::DEALT_WITH;\n}\n"
        + "ActionResult View::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {\n"
        + "return sharedSaveLoadBranch(b, on, inCardRoutine);\n}\n"
        + "using namespace deluge::hid::button;\n"
        + root_handler
        + args.tests.read_text()
    )


if __name__ == "__main__":
    main()
