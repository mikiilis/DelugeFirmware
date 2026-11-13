"""Pin the demo's literal palette and browser-coordinate ring without dependencies."""

import ast
import re
import unittest
from pathlib import Path

DEMO = Path(__file__).resolve().parents[3] / "website/public/demos/loop-station.html"


def literal_array(source, name):
    markers = list(re.finditer(r"\bconst\s+" + re.escape(name) + r"\s*=\s*", source))
    if len(markers) != 1:
        raise ValueError(f"missing or ambiguous array marker: {name}")
    start = markers[0].end()
    if source[start : start + 1] != "[":
        raise ValueError(f"not a literal array: {name}")
    depth = 0
    for end in range(start, len(source)):
        if source[end] == "[":
            depth += 1
        elif source[end] == "]":
            depth -= 1
            if depth == 0:
                return ast.literal_eval(source[start : end + 1])
    raise ValueError(f"unterminated array: {name}")


class LoopStationDemoContract(unittest.TestCase):
    def test_eight_distinct_chromatic_defaults(self):
        colors = literal_array(DEMO.read_text(), "colors")
        self.assertEqual(8, len(colors))
        rgb = []
        for color in colors:
            self.assertRegex(color, r"^#[0-9a-fA-F]{6}$")
            channels = tuple(int(color[i : i + 2], 16) for i in (1, 3, 5))
            self.assertGreaterEqual(max(channels) - min(channels), 64, color)
            rgb.append(channels)
        self.assertEqual(8, len(set(rgb)))
        self.assertEqual("#ff0080", colors[5])

    def test_top_left_clockwise_ring_unchanged(self):
        self.assertEqual(
            [
                [0, 0],
                [1, 0],
                [2, 0],
                [3, 0],
                [3, 1],
                [3, 2],
                [3, 3],
                [2, 3],
                [1, 3],
                [0, 3],
                [0, 2],
                [0, 1],
            ],
            literal_array(DEMO.read_text(), "ring"),
        )


if __name__ == "__main__":
    unittest.main()
