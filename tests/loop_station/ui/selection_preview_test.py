"""Exercise the demo's selection-display helpers in an available JavaScript runtime."""

import json
import re
import shutil
import subprocess
import unittest
from pathlib import Path

DEMO = Path(__file__).resolve().parents[3] / "website/public/demos/loop-station.html"


def function_body(source, name):
    match = re.search(r"\bfunction\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source)
    if not match:
        raise AssertionError(f"missing demo behavior: {name}")
    depth = 1
    for end in range(match.end(), len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[match.start() : end + 1]
    raise AssertionError(f"unterminated demo behavior: {name}")


def run_demo_helpers(expression):
    source = DEMO.read_text()
    code = "\n".join(
        function_body(source, name)
        for name in ("selectedTrackTitle", "nextSelection", "selectionBrightness")
    )
    if shutil.which("node"):
        command = [
            "node",
            "-e",
            code + "\nconsole.log(JSON.stringify(" + expression + "))",
        ]
    elif shutil.which("osascript"):
        command = [
            "osascript",
            "-l",
            "JavaScript",
            "-e",
            code + "\nJSON.stringify(" + expression + ")",
        ]
    else:
        raise unittest.SkipTest("No JavaScript runtime available (node or osascript)")
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


class SelectionPreviewTests(unittest.TestCase):
    def test_selected_track_title_uses_one_based_number(self):
        self.assertEqual(
            ["TRACK 1", "TRACK 8"],
            run_demo_helpers("[selectedTrackTitle(0), selectedTrackTitle(7)]"),
        )

    def test_border_press_toggles_selection(self):
        self.assertEqual(
            [2, -1, 2, -1],
            run_demo_helpers(
                "[nextSelection(0,2),nextSelection(2,2),nextSelection(-1,2),nextSelection(2,2)]"
            ),
        )

    def test_only_selected_ordinary_ring_breathes(self):
        # Args: track index, selected index, state, the existing REC/DUB pulse, filled, reduced motion.
        samples = run_demo_helpers(
            "["
            "selectionBrightness(1,1,'playing',0.25,true,false),"
            "selectionBrightness(1,1,'playing',0.85,true,false),"
            "selectionBrightness(1,1,'playing',0.25,false,false),"
            "selectionBrightness(1,1,'playing',0.85,false,false),"
            "selectionBrightness(1,0,'playing',0.55,true,false),"
            "selectionBrightness(1,1,'recording',0.55,true,false),"
            "selectionBrightness(1,1,'overdubbing',0.55,true,false),"
            "selectionBrightness(1,1,'empty',0.25,false,false),"
            "selectionBrightness(1,1,'empty',0.85,false,false),"
            "selectionBrightness(1,1,'stopped',0.55,false,false),"
            "selectionBrightness(1,1,'playing',0.25,true,true),"
            "selectionBrightness(1,1,'playing',0.85,true,true)"
            "]"
        )
        (
            bright_low,
            bright_high,
            dim_low,
            dim_high,
            other,
            record,
            overdub,
            empty_low,
            empty_high,
            stopped,
            static_a,
            static_b,
        ) = samples
        self.assertLess(bright_low, bright_high)
        self.assertLess(dim_low, dim_high)
        self.assertGreater(bright_low, dim_low)
        self.assertGreater(bright_high, dim_high)
        self.assertIsNone(other)
        self.assertIsNone(record)
        self.assertIsNone(overdub)
        self.assertLess(empty_low, empty_high)
        self.assertIsNotNone(stopped)
        self.assertEqual(static_a, static_b)

    def test_selection_brightness_uses_existing_record_pulse_range(self):
        low, high = run_demo_helpers(
            "[selectionBrightness(0,0,'empty',0.25,false,false),"
            "selectionBrightness(0,0,'empty',0.85,false,false)]"
        )
        self.assertAlmostEqual(0.14, low)
        self.assertAlmostEqual(0.5, high)


if __name__ == "__main__":
    unittest.main()
