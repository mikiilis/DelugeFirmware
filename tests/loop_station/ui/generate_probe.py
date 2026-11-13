"""Compile checked, unmodified UI source sections against firmware boundary stubs."""

import argparse
from pathlib import Path


def section(source: str, begin: str, end: str) -> str:
    start = source.find(begin)
    if start < 0 or source.find(begin, start + len(begin)) >= 0:
        raise SystemExit(f"production source marker missing or ambiguous: {begin!r}")
    finish = source.find(end, start + len(begin))
    if finish < 0:
        raise SystemExit(f"production source end marker not found: {end!r}")
    return source[start:finish]


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in ("loop-view", "settings-view", "support", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    loop = args.loop_view.read_text()
    settings = args.settings_view.read_text()
    encoder_marker = "void LoopStationView::selectEncoderAction("
    if loop.count(encoder_marker) != 1 or not loop.rstrip().endswith("}"):
        raise SystemExit("production encoder definition missing or ambiguous")
    text = "\n".join(
        [
            f'#include "{args.support.as_posix()}"',
            "using namespace deluge::hid::button;\nusing namespace loop_station;",
            section(loop, "namespace {", "bool LoopStationView::opened()"),
            section(
                loop,
                "bool LoopStationView::opened()",
                "void LoopStationView::renderOLED(",
            ),
            loop[loop.index(encoder_marker) :],
            "namespace deluge::gui::menu_item::loop_station {",
            "using namespace ::loop_station;",
            section(
                settings,
                "namespace {\nSongModel* currentModel()",
                "bool SettingsMenu::renderMainPads(",
            ),
            section(
                settings,
                "void SettingsMenu::renderOLED(",
                "} // namespace deluge::gui::menu_item::loop_station",
            ),
            "}",
        ]
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)


if __name__ == "__main__":
    main()
