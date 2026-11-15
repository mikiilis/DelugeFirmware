#!/usr/bin/env python3
"""Extract the unchanged production Song::serviceLoopStationArchive body and
compile it against stubs, so the real foreground scheduling owner is exercised.

A test that calls only SongModel::serviceHistoryNavigation cannot prove the
firmware archive tick actually schedules it. Renaming the production function to
ProbeSong keeps its exact body while the support header supplies the only symbols
it touches.
"""

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
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--support", type=Path, required=True)
    parser.add_argument("--tests", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    body = section(
        args.firmware.read_text(),
        "void Song::serviceLoopStationArchive(",
        "loop_station::Status Song::ensureLoopStation(",
    ).replace(
        "Song::serviceLoopStationArchive(", "ProbeSong::serviceLoopStationArchive(", 1
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        f'#include "{args.support.as_posix()}"\n' + body + "\n" + args.tests.read_text()
    )


if __name__ == "__main__":
    main()
