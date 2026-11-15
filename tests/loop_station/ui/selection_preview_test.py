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


def run_demo_helpers(expression, helpers=None, preamble=""):
    source = DEMO.read_text()
    if helpers is None:
        helpers = ("selectedTrackTitle", "nextSelection", "selectionBrightness")
    code = preamble + "\n".join(function_body(source, name) for name in helpers)
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

    def test_bars_text_reports_measured_length(self):
        self.assertEqual(
            [
                "READY",
                "REC",
                "REC",
                "1/16 BAR",
                "0.37 s",
                "1 BAR",
                "3/2 BARS",
                "4 BARS",
            ],
            run_demo_helpers(
                "[barsText({state:'empty',length:0}),"
                "barsText({state:'recording',length:0.37}),"
                "barsText({state:'overdubbing',length:0.37}),"
                "barsText({state:'playing',length:0.125}),"
                "barsText({state:'stopped',length:0.37}),"
                "barsText({state:'playing',length:2}),"
                "barsText({state:'playing',length:3}),"
                "barsText({state:'playing',length:8})]",
                helpers=("barsText", "formatPhraseLength"),
                preamble="const tempoBPM = 120;\n",
            ),
        )

    def test_length_formatter_matches_firmware_contract(self):
        # Literal cases independently checked against 44.1-kHz durations; do not
        # compute expected labels with a second implementation of the formatter.
        musical_cases = [
            (0, "0.00 s"),
            (1, "<0.01 s"),
            (220, "<0.01 s"),
            (221, "0.01 s"),
            (1838, "1/48 BAR"),
            (2757, "1/32 BAR"),
            (3675, "1/24 BAR"),
            (5513, "1/16 BAR"),
            (7350, "1/12 BAR"),
            (11025, "1/8 BAR"),
            (14700, "1/6 BAR"),
            (16538, "3/16 BAR"),
            (22050, "1/4 BAR"),
            (29400, "1/3 BAR"),
            (44100, "1/2 BAR"),
            (66150, "3/4 BAR"),
            (88200, "1 BAR"),
            (132300, "3/2 BARS"),
            (352800, "4 BARS"),
            (16317, "0.37 s"),
            (5511, "0.12 s"),
            (5512, "1/16 BAR"),
            (5514, "1/16 BAR"),
            (5515, "0.13 s"),
            (330750, "7.50 s"),
        ]
        cases = [(frames, 120 * 65536, label) for frames, label in musical_cases]
        cases += [
            (5880, 112 * 65536 + 32768, "1/16 BAR"),
            (5513, 121 * 65536, "0.13 s"),
            (16317, 60 * 65536, "0.37 s"),
            (16317, 120 * 65536, "0.37 s"),
            (439, 0, "0.01 s"),
            (44099, 0, "1.00 s"),
            (44099779, 0, "999.99 s"),
            (44099780, 0, "1000 s"),
            (4294967295, 0, "97392 s"),
            (4294967295, 1000 * 65536, "97392 s"),
            (16317, 1001 * 65536, "0.37 s"),
            (16317, 1, "0.37 s"),
        ]
        inputs = json.dumps([[frames, tempo] for frames, tempo, _ in cases])
        results = run_demo_helpers(
            inputs + ".map(([frames, tempo]) => formatPhraseLength(frames, tempo))",
            helpers=("formatPhraseLength",),
        )
        self.assertEqual([label for _, _, label in cases], results)
        self.assertTrue(all(len(label) <= 8 for label in results))

    def test_global_settings_render_without_a_selected_track(self):
        # No DOM runner is available, so exercise the render loop against a minimal
        # element stub: with selected = -1 it must not throw and must still show
        # the global fields, and the track lookup must stay scope-guarded.
        preamble = """
let selected = -1
let settingsScope = "global"
let settingIndex = 0
let confirmingCopy = false
let copyTarget = 0
const tracks = []
const shiftActive = () => false
const sourceOptions = ["Left", "Right", "Stereo", "Balanced"]
const colourOptions = [["Blue", "#6098e8"]]
const intervals = ["1/32", "1/32T", "1/16", "1/16T", "1/8", "1/8T", "1/4", "1/4T", "1/2", "1/2T", "Bar"]
const settingNames = {}
const globalSettings = { recordAction: 0, startMode: "Synced", startInterval: 10, stopMode: "Synced", stopInterval: 10 }
const elements = {}
function element(id) {
  if (!(id in elements))
    elements[id] = {
      id,
      textContent: "",
      disabled: false,
      children: [],
      classList: { toggle() {} },
      setAttribute() {},
      replaceChildren() { this.children = [] },
      appendChild(child) { this.children.push(child) },
    }
  return elements[id]
}
const document = {
  querySelector: () => element("oled"),
  querySelectorAll: () => [],
  getElementById: element,
  createElement: () => ({
    children: [],
    style: {},
    textContent: "",
    append(...kids) { this.children.push(...kids) },
  }),
}
"""
        (
            _rendered,
            title,
            back_disabled,
            values,
            global_lookup,
            track_lookup,
        ) = run_demo_helpers(
            "[renderSettings(),"
            "elements['oled-title'].textContent,"
            "elements['back'].disabled,"
            "elements['settings-columns'].children.map((cell) => cell.children[1].textContent),"
            "trackSettingsFor('global'),"
            "(tracks.push({settings:{source:2}}), selected = 0, trackSettingsFor('track').source)]",
            helpers=(
                "renderSettings",
                "settingFields",
                "settingValue",
                "trackSettingsFor",
            ),
            preamble=preamble,
        )
        self.assertEqual("GLOBAL SETTINGS", title)
        self.assertFalse(back_disabled)
        self.assertEqual(["R P D", "Synced", "Bar", "Synced"], values)
        self.assertIsNone(global_lookup)
        self.assertEqual(2, track_lookup)

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
