"""Compile unchanged production graphics dispatch functions against hardware stubs."""

import argparse
from pathlib import Path


def section(source, start, end):
    if source.count(start) != 1:
        raise SystemExit(f"missing or ambiguous production dispatch marker: {start}")
    first = source.index(start)
    last = source.find(end, first + len(start))
    if last < 0:
        raise SystemExit(f"missing production dispatch end marker: {end}")
    return source[first:last]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ui", type=Path, required=True)
    parser.add_argument("--timers", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    ui = args.ui.read_text()
    timers = args.timers.read_text()
    rendering = section(
        ui,
        "uint32_t whichMainRowsNeedRendering = 0;",
        "void doAnyPendingOLEDRendering()",
    )
    timer_case = section(
        timers, "case TimerName::GRAPHICS_ROUTINE:", "case TimerName::OLED_CONSOLE:"
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        '#include "settings_probe_support.h"\n'
        "namespace production_dispatch {\n"
        "using UI = ProbeUI;\n"
        "static std::array<UI*, 2> uiNavigationHierarchy{};\n"
        "static int32_t numUIsOpen = 0;\n"
        "static int currentUIMode = 0;\n"
        "constexpr int UI_MODE_HORIZONTAL_SCROLL = 1, UI_MODE_HORIZONTAL_ZOOM = 2;\n"
        "namespace PadLEDs {\n"
        "RGB image[kDisplayHeight][kDisplayWidth + kSideBarWidth]{};\n"
        "uint8_t occupancyMask[kDisplayHeight][kDisplayWidth + kSideBarWidth]{};\n"
        "namespace horizontal { bool isSidebarScrolling() { return false; } }\n"
        "void sendOutMainPadColours() { probeSentPadFrames.push_back({image[3][4], image[7][0]}); }\n"
        "void sendOutSidebarColours() {}\n"
        "}\n" + rendering + "enum class TimerName { GRAPHICS_ROUTINE };\n"
        "constexpr int UART_ITEM_PIC_PADS = 0, kNumBytesInColUpdateMessage = 1;\n"
        "static bool uartReady = true;\n"
        "int uartGetTxBufferSpace(int) { return uartReady ? 2 : 0; }\n"
        "void setTimer(TimerName, int) {}\n"
        "void dispatchTick(bool ready) {\n"
        "uartReady = ready;\n"
        "switch (TimerName::GRAPHICS_ROUTINE) {\n" + timer_case + "default: break;\n"
        "}\n"
        "doAnyPendingGridRendering();\n"
        "}\n"
        "void setHierarchy(UI* root, UI* current) {\n"
        "uiNavigationHierarchy = {root, current == root ? nullptr : current};\n"
        "numUIsOpen = current == root ? 1 : 2;\n"
        "clearPendingUIRendering();\n"
        "}\n"
        "}\n"
    )


if __name__ == "__main__":
    main()
