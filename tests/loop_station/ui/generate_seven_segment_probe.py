"""Extract the unchanged production seven-segment encoding/scrolling/layer code.

This compiles the real SevenSegment encoder, the real NumericLayerScrollingText
progression and the real layer lifecycle so a full track/action/instruction
string can be shown to encode and scroll rather than truncate to four characters.
"""

import argparse
from pathlib import Path


def section(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1:
        raise SystemExit(f"production marker missing or ambiguous: {begin!r}")
    start = source.index(begin)
    finish = source.find(end, start + len(begin))
    if finish < 0:
        raise SystemExit(f"production end marker not found: {end!r}")
    return source[start:finish]


def section_to_end(source: str, begin: str) -> str:
    if source.count(begin) != 1:
        raise SystemExit(f"production marker missing or ambiguous: {begin!r}")
    return source[source.index(begin) :]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seven", type=Path, required=True)
    parser.add_argument("--numeric-layer", type=Path, required=True)
    parser.add_argument("--basic-layer", type=Path, required=True)
    parser.add_argument("--scrolling-layer", type=Path, required=True)
    parser.add_argument("--support", type=Path, required=True)
    parser.add_argument("--tests", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    seven = args.seven.read_text()
    view_code = "\n".join(
        [
            section(
                seven, "uint8_t numberSegments[10] =", "void SevenSegment::setTopLayer("
            ),
            section(
                seven,
                "void SevenSegment::setTopLayer(",
                "void SevenSegment::setTextWithMultipleDots(",
            ),
            section(
                seven,
                "void SevenSegment::setTextWithMultipleDots(",
                "NumericLayerScrollingText* SevenSegment::setScrollingText(",
            ),
            section(
                seven,
                "NumericLayerScrollingText* SevenSegment::setScrollingText(",
                "int32_t SevenSegment::getEncodedPosFromLeft(",
            ),
            section(
                seven,
                "int32_t SevenSegment::getEncodedPosFromLeft(",
                "int32_t SevenSegment::encodeText(",
            ),
            section(
                seven,
                "int32_t SevenSegment::encodeText(",
                "void SevenSegment::setTextAsNumber(",
            ),
        ]
    )

    numeric_layer = section_to_end(
        args.numeric_layer.read_text(), "NumericLayer::~NumericLayer()"
    )
    basic_layer = section_to_end(
        args.basic_layer.read_text(), "NumericLayerBasicText::NumericLayerBasicText("
    )
    scrolling_layer = section_to_end(
        args.scrolling_layer.read_text(),
        "NumericLayerScrollingText::NumericLayerScrollingText(",
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        f'#include "{args.support.as_posix()}"\n'
        "namespace deluge::hid::display {\n"
        + view_code
        + "\n} // namespace deluge::hid::display\n\n"
        + numeric_layer
        + "\n"
        + basic_layer
        + "\n"
        + scrolling_layer
        + "\n"
        + args.tests.read_text()
    )


if __name__ == "__main__":
    main()
