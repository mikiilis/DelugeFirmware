#!/usr/bin/env python3
"""Extract the real FirmwareOwner checkpoint-mask methods for real-model tests."""

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
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    firmware = args.firmware.read_text()
    methods = []
    for begin, end in (
        ("\tvoid beginCheckpointRestores(", "\tvoid startNextRestore("),
        ("\tvoid startNextRestore(", "\tvoid serviceCheckpointRestore("),
        ("\tvoid serviceCheckpointRestore(", "\tvoid* allocatePinnedLowSpeed("),
    ):
        text = section(firmware, begin, end)
        signature = begin.lstrip()
        text = text.replace(
            signature, "void CheckpointOwnerProbe::" + signature[len("void ") :], 1
        )
        methods.append(text)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        f'#include "{args.support.as_posix()}"\n'
        "namespace loop_station {\n"
        + "\n".join(methods)
        + "\n} // namespace loop_station\n"
    )


if __name__ == "__main__":
    main()
