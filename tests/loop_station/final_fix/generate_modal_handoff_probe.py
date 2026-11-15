#!/usr/bin/env python3
"""Extract unchanged physical matrix and LoadSongUI pad dispatch bodies."""

from __future__ import annotations

import argparse
from pathlib import Path


def extract(source: str, begin: str, end: str | None = None) -> str:
    if source.count(begin) != 1:
        raise SystemExit(f"missing or ambiguous production marker: {begin!r}")
    start = source.index(begin)
    if end is None:
        if not source.rstrip().endswith("}"):
            raise SystemExit("expected production final method at end of source")
        return source[start : source.rfind("}") + 1]
    finish = source.find(end, start + len(begin))
    if finish < 0:
        raise SystemExit(f"missing production end marker: {end!r}")
    return source[start:finish]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--load-ui", type=Path, required=True)
    parser.add_argument("--support", type=Path, required=True)
    parser.add_argument("--tests", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    matrix = extract(
        args.matrix.read_text(),
        "ActionResult MatrixDriver::padAction(",
        "bool MatrixDriver::isPadPressed(",
    )
    load = extract(args.load_ui.read_text(), "ActionResult LoadSongUI::padAction(")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        f'#include "{args.support.as_posix()}"\n'
        "StemExport stemExport{};\nUI* currentUI = nullptr;\n"
        + matrix
        + load
        + "\n"
        + args.tests.read_text()
    )


if __name__ == "__main__":
    main()
