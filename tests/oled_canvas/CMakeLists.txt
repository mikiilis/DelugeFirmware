find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(OLED_CANVAS_PROBE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(OLED_CANVAS_PROBE "${OLED_CANVAS_PROBE_DIR}/production_canvas.cpp")
set(OLED_VIEWS_PROBE "${OLED_CANVAS_PROBE_DIR}/production_views.cpp")
set(OLED_CANVAS_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/generate_probe.py")
set(OLED_CANVAS_SUPPORT "${CMAKE_CURRENT_SOURCE_DIR}/probe_support.h")
set(OLED_CANVAS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/hid/display/oled_canvas/canvas.cpp")
set(OLED_CANVAS_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/hid/display/oled_canvas/canvas.h")
set(OLED_CANVAS_FONTS "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/fonts/fonts.c")
set(OLED_CANVAS_FONTS_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/fonts/fonts.h")
set(LOOP_STATION_VIEW "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/views/loop_station_view.cpp")
set(LOOP_STATION_SETTINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/menu_item/loop_station/settings.cpp")
set(LOOP_STATION_MENU_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/loop_station/menu_model.cpp")

add_custom_command(
    OUTPUT "${OLED_CANVAS_PROBE}" "${OLED_VIEWS_PROBE}"
    COMMAND "${Python3_EXECUTABLE}" "${OLED_CANVAS_GENERATOR}"
            --source "${OLED_CANVAS_SOURCE}"
            --header "${OLED_CANVAS_HEADER}"
            --support "${OLED_CANVAS_SUPPORT}"
            --output "${OLED_CANVAS_PROBE}"
            --views-output "${OLED_VIEWS_PROBE}"
            --loop-view "${LOOP_STATION_VIEW}"
            --settings-view "${LOOP_STATION_SETTINGS}"
            --menu-model "${LOOP_STATION_MENU_MODEL}"
    DEPENDS "${OLED_CANVAS_GENERATOR}" "${OLED_CANVAS_SUPPORT}"
            "${OLED_CANVAS_SOURCE}" "${OLED_CANVAS_HEADER}"
            "${OLED_CANVAS_FONTS_HEADER}"
            "${LOOP_STATION_VIEW}" "${LOOP_STATION_SETTINGS}" "${LOOP_STATION_MENU_MODEL}"
    VERBATIM
)

add_executable(OledCanvasTests canvas_bounds_tests.cpp "${OLED_CANVAS_PROBE}" "${OLED_CANVAS_FONTS}")
target_include_directories(OledCanvasTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge")
set_target_properties(OledCanvasTests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_compile_options(OledCanvasTests PRIVATE -Wall -Wextra)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(OledCanvasTests PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
    target_link_options(OledCanvasTests PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
endif()
add_test(NAME OledCanvasTests COMMAND OledCanvasTests)

add_executable(LoopStationOledTests loop_station_oled_tests.cpp
    "${OLED_CANVAS_PROBE}" "${OLED_VIEWS_PROBE}" "${OLED_CANVAS_FONTS}")
target_include_directories(LoopStationOledTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge")
set_target_properties(LoopStationOledTests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_compile_options(LoopStationOledTests PRIVATE -Wall -Wextra)
add_test(NAME LoopStationOledTests COMMAND LoopStationOledTests)
