"""Extract the unchanged production tempo-encoder dispatch against narrow stubs.

The physical dispatcher reaches the tempo encoder through encoder_input.cpp, which
bypasses the per-view encoder handlers. This compiles that exact case body so the
unrelated-action notification and the native tempo/clip routing are exercised.
"""

import argparse
from pathlib import Path


def section(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1:
        raise SystemExit(f"production encoder marker missing or ambiguous: {begin!r}")
    start = source.index(begin)
    finish = source.find(end, start + len(begin))
    if finish < 0:
        raise SystemExit(f"production encoder end marker not found: {end!r}")
    return source[start:finish]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--encoder", type=Path, required=True)
    parser.add_argument("--support", type=Path, required=True)
    parser.add_argument("--tests", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    body = section(
        args.encoder.read_text(),
        "case 2: // tempo",
        "case 3: // select",
    )
    # Drop the case label and the trailing break so the body becomes a function.
    body = body[body.index("\n") + 1 :]
    body = body.rstrip()
    if not body.endswith("break;"):
        raise SystemExit("unexpected tempo case terminator")
    body = body[: body.rfind("break;")]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        f'#include "{args.support.as_posix()}"\n'
        "\n"
        "void tempoEncoderDispatch(int8_t saturatedDelta) {\n"
        + body
        + "}\n\n"
        + args.tests.read_text()
    )


if __name__ == "__main__":
    main()
