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


if __name__ == "__main__":
    main()
