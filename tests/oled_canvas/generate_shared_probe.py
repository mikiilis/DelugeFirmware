"""Real fonts/canvas for the integrated production-method UI probe.

Four names are macro-aliased to retain the existing text/highlight/pixel spies;
the extracted production bodies are unchanged.
"""

import sys
from pathlib import Path

from generate_probe import section

root = Path(__file__).resolve().parents[2]
source = (root / "src/deluge/hid/display/oled_canvas/canvas.cpp").read_text()
ranges = [
    ("namespace {\nconstexpr int32_t kMaxX", "void Canvas::clearAreaExact"),
    ("void Canvas::drawPixel(", "void Canvas::drawVerticalLine("),
    ("void Canvas::drawLine(", "void Canvas::drawRectangle("),
    ("void Canvas::drawString(std::string_view", "void Canvas::drawStringCentred("),
    ("void Canvas::drawStringCentered(char", "/// Draw a string, reducing"),
    ("void Canvas::drawChar(", "void Canvas::drawIcon("),
    ("void Canvas::drawScreenTitle(", "/// inverts just the left edge"),
]
text = '#include "shared_probe_support.h"\nusing deluge::hid::display::oled_canvas::Canvas;\n'
text += "using namespace deluge::hid::display::oled_canvas;\n#define DO_CHARACTER_SCALING 0\n"
for begin, end in ranges:
    if begin.startswith("void Canvas::drawPixel("):
        text += "#define drawPixel drawPixelPixels\n#define drawHorizontalLine drawHorizontalLinePixels\n"
    if begin.startswith("void Canvas::drawString(std"):
        text += "#define drawString drawStringPixels\n"
    if begin.startswith("void Canvas::drawScreenTitle"):
        text += "#define invertAreaRounded invertAreaRoundedPixels\n"
    text += section(source, begin, end) + "\n"
    text += "#undef drawString\n#undef invertAreaRounded\n#undef drawPixel\n#undef drawHorizontalLine\n"
Path(sys.argv[1]).write_text(text)
