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
            f'#include "{args.support.parent.as_posix()}/settings_probe_support.h"',
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

    # Shared-menu declarations and bodies are taken from production, never reimplemented.
    menu_dir = args.settings_view.parent.parent
    declarations = []
    for name in ("submenu", "horizontal_menu"):
        header = (menu_dir / f"{name}.h").read_text()
        declarations.append(
            "\n".join(
                line
                for line in header.splitlines()
                if not line.startswith("#include") and line != "#pragma once"
            )
        )
    (args.output.parent / "shared_menu_declarations.h").write_text(
        "\n".join(declarations)
    )
    submenu = (menu_dir / "submenu.cpp").read_text()
    horizontal = (menu_dir / "horizontal_menu.cpp").read_text()
    decimal = (menu_dir / "decimal.cpp").read_text()
    menu_item = (menu_dir / "menu_item.cpp").read_text()
    value = (menu_dir / "value.h").read_text()
    bodies = [
        section(
            menu_item + "\n// probe end", "void MenuItem::endSession", "// probe end"
        ),
        section(
            value,
            "template <typename T>\nvoid Value<T>::selectEncoderAction",
            "template <typename T>\nvoid Value<T>::readValueAgain",
        ),
        section(
            submenu,
            "bool Submenu::isChildRelevant",
            "void Submenu::renderInHorizontalMenu",
        ),
        section(submenu, "bool Submenu::wrapAround", "void Submenu::unlearnAction"),
        section(
            submenu, "void Submenu::updatePadLights", "bool Submenu::usesAffectEntire"
        ),
        section(
            horizontal,
            "void HorizontalMenu::beginSession",
            "} // namespace deluge::gui::menu_item",
        ),
        section(decimal, "int32_t Decimal::getNumberEditSize", "bool movingCursor"),
        section(
            decimal, "void Decimal::scrollToGoodPos", "void Decimal::drawPixelsForOled"
        ),
        section(
            decimal,
            "void Decimal::renderInHorizontalMenu",
            "void DecimalWithoutScrolling::selectEncoderAction",
        ),
    ]
    (args.output.parent / "shared_menu.cpp").write_text(
        f'#include "{args.support.parent.as_posix()}/shared_probe_support.h"\n'
        "namespace deluge::gui::menu_item {\nusing namespace hid::display;\n"
        + "\n".join(bodies)
        + "\n}\n"
    )

    # Native items use the same production declaration/body extraction boundary.
    native_dir = args.settings_view.parent
    (args.output.parent / "horizontal_settings_declarations.h").write_text(
        '#pragma once\n#include "settings_items_declarations.h"\n'
        + "\n".join(
            line
            for line in (native_dir / "horizontal_settings.h").read_text().splitlines()
            if not line.startswith("#include") and line != "#pragma once"
        )
    )
    (args.output.parent / "horizontal_settings.cpp").write_text(
        f'#include "{args.support.parent.as_posix()}/settings_probe_support.h"\n'
        + "\n".join(
            line
            for line in (native_dir / "horizontal_settings.cpp")
            .read_text()
            .splitlines()
            if not line.startswith("#include")
        )
    )
    (args.output.parent / "settings_items_declarations.h").write_text(
        "#pragma once\n"
        + f'#include "{args.support.parent.as_posix()}/shared_probe_support.h"\n'
        + "\n".join(
            line
            for line in (native_dir / "settings_items.h").read_text().splitlines()
            if not line.startswith("#include") and line != "#pragma once"
        )
    )
    (args.output.parent / "settings_items.cpp").write_text(
        f'#include "{args.support.parent.as_posix()}/shared_probe_support.h"\n'
        '#include "settings_items_declarations.h"\n'
        + "\n".join(
            line
            for line in (native_dir / "settings_items.cpp").read_text().splitlines()
            if not line.startswith("#include")
        )
    )


if __name__ == "__main__":
    main()
