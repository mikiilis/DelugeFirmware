"""Extract the unchanged stutter/retarget cleanup methods against narrow test boundaries."""

import argparse
from pathlib import Path


def extract(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1:
        raise SystemExit(f"production lifecycle marker missing or ambiguous: {begin!r}")
    start = source.index(begin)
    finish = source.find(end, start + len(begin))
    if finish < 0:
        raise SystemExit(f"production lifecycle end marker not found: {end!r}")
    return source[start:finish]


def extract_final_method(source: str, begin: str) -> str:
    if source.count(begin) != 1 or not source.rstrip().endswith("}"):
        raise SystemExit(
            f"production lifecycle final method missing or ambiguous: {begin!r}"
        )
    return source[source.index(begin) :]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stutterer", type=Path, required=True)
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--view", type=Path, required=True)
    parser.add_argument("--support", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    stutterer = args.stutterer.read_text()
    audio = args.audio.read_text()
    view = args.view.read_text()
    if stutterer.count("Error Stutterer::beginStutter(") != 1:
        raise SystemExit(
            "production Stutterer::beginStutter marker missing or ambiguous"
        )
    text = "\n".join(
        [
            f'#include "{args.support.parent.as_posix()}/settings_probe_support.h"',
            '#include "stutter_lifecycle_support.h"',
            "#define ModControllableAudio LifecycleModControllableAudio",
            "#define View LifecycleView",
            "namespace params = deluge::modulation::params;",
            "Stutterer stutterer{};",
            "int32_t paramNeutralValues[params::kNumParams]{};",
            "int32_t getFinalParameterValueExp(int32_t, int32_t) { return 0x40000000; }",
            "int32_t cableToExpParamShortcut(int32_t value) { return value >> 2; }",
            extract(
                stutterer,
                "int32_t Stutterer::getStutterRate(",
                "void Stutterer::processStutter(",
            ),
            extract_final_method(stutterer, "void Stutterer::endStutter("),
            extract(
                audio,
                "void ModControllableAudio::endStutter(",
                "void ModControllableAudio::switchDelayPingPong(",
            ),
            extract(
                view,
                "bool View::finishActiveStutterForModTargetChange(",
                "void View::setActiveModControllableWithoutTimelineCounter(",
            ),
        ]
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)


if __name__ == "__main__":
    main()
