"""Build a host translation unit from unmodified production OLED canvas routines."""

import argparse
from pathlib import Path


def section(source: str, begin: str, end: str) -> str:
    start = source.find(begin)
    if start < 0:
        raise SystemExit(f"production source marker not found: {begin!r}")
    finish = source.find(end, start + len(begin))
    if finish < 0:
        raise SystemExit(f"production source end marker not found: {end!r}")
    return source[start:finish]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--support", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--views-output", type=Path, required=True)
    parser.add_argument("--loop-view", type=Path, required=True)
    parser.add_argument("--settings-view", type=Path, required=True)
    parser.add_argument("--menu-model", type=Path, required=True)
    args = parser.parse_args()

    canvas = args.source.read_text()
    functions = [
        section(
            canvas,
            "void Canvas::drawString(std::string_view",
            "void Canvas::drawStringCentred(",
        ),
        section(canvas, "void Canvas::drawChar(", "int32_t Canvas::getCharIndex("),
        section(
            canvas,
            "int32_t Canvas::getCharIndex(",
            "int32_t Canvas::getCharWidthInPixels(",
        ),
        section(
            canvas,
            "int32_t Canvas::getCharWidthInPixels(",
            "int32_t Canvas::getCharSpacingInPixels(",
        ),
        section(
            canvas,
            "int32_t Canvas::getCharSpacingInPixels(",
            "namespace {\nstruct KerningRule",
        ),
        section(
            canvas,
            "namespace {\nstruct KerningRule",
            "// Calculate the width of a string",
        ),
        section(
            canvas,
            "int32_t Canvas::getStringWidthInPixels(",
            "void Canvas::drawGraphicMultiLine(",
        ),
        section(canvas, "void Canvas::drawGraphicMultiLine(", "void Canvas::drawIcon("),
    ]

    text = "\n".join(
        [
            "#include <algorithm>\n#include <array>\n#include <cstdint>\n#include <string_view>\n#include <vector>",
            f'#include "{args.support.as_posix()}"',
            "using deluge::hid::display::oled_canvas::Canvas;",
            "namespace { int32_t getPreviousCharSpacingAdjustmentInPixels(uint8_t, uint8_t, int32_t); }",
            "#define DO_CHARACTER_SCALING 0",
            *functions,
        ]
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)

    loop_view = args.loop_view.read_text()
    loop_helpers = section(loop_view, "SongModel* model()", "RGB colourFor(")
    loop_method = section(
        loop_view,
        "void LoopStationView::renderOLED(",
        "void LoopStationView::selectEncoderAction(",
    )
    loop_method = loop_method.replace(
        "kConfiguredMemoryProfile", "testConfiguredMemoryProfile"
    )

    settings_view = args.settings_view.read_text()
    settings_helpers = section(
        settings_view,
        "namespace {\nSongModel* currentModel()",
        "bool SettingsMenu::openGlobal()",
    )
    settings_method = section(
        settings_view,
        "void SettingsMenu::renderListOLED(",
        "void SettingsMenu::closeMenu()",
    )
    menu_model = args.menu_model.read_text()
    visible_fields = section(
        menu_model,
        "MenuFields visibleMenuFields(",
        "uint16_t maxDurationBars(",
    )
    views_text = "\n".join(
        [
            "#include <algorithm>\n#include <array>\n#include <cstdio>\n#include <cstdint>",
            f'#include "{args.support.as_posix()}"',
            "using namespace loop_station;",
            "namespace {",
            loop_helpers,
            "}",
            loop_method,
            "namespace loop_station {",
            visible_fields,
            "}",
            "namespace deluge::gui::menu_item::loop_station {",
            "using namespace ::loop_station;",
            settings_helpers,
            settings_method,
            "}",
        ]
    )
    args.views_output.parent.mkdir(parents=True, exist_ok=True)
    args.views_output.write_text(views_text)


if __name__ == "__main__":
    main()
