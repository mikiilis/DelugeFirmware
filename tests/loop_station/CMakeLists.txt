find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(LOOP_UI_PROBE "${CMAKE_CURRENT_BINARY_DIR}/generated/production_ui.cpp")
set(LOOP_UI_VIEW "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/views/loop_station_view.cpp")
set(LOOP_UI_SETTINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/menu_item/loop_station/settings.cpp")
set(STUTTER_LIFECYCLE_PROBE "${CMAKE_CURRENT_BINARY_DIR}/generated/production_stutter_lifecycle.cpp")
set(LOOP_DISPATCH_PROBE "${CMAKE_CURRENT_BINARY_DIR}/generated/production_dispatch.cpp")
set(FINAL_TARGET_STACK_PROBE "${CMAKE_CURRENT_BINARY_DIR}/generated/final_target_stack_probe.cpp")
add_custom_command(
    OUTPUT "${FINAL_TARGET_STACK_PROBE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/generate_target_stack_probe.py"
            --view "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/views/view.cpp"
            --song "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/model/song/song.cpp"
            --loop-view "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/views/loop_station_view.cpp"
            --session-view "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/views/session_view.cpp"
            --audio-view "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/views/audio_clip_view.cpp"
            --support "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/target_stack_probe_support.h"
            --tests "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/target_stack_probe_tests.cpp"
            --output "${FINAL_TARGET_STACK_PROBE}"
    DEPENDS final_fix/generate_target_stack_probe.py final_fix/target_stack_probe_support.h
            final_fix/target_stack_probe_tests.cpp ../../src/deluge/gui/views/view.cpp
            ../../src/deluge/model/song/song.cpp ../../src/deluge/gui/views/loop_station_view.cpp
            ../../src/deluge/gui/views/session_view.cpp ../../src/deluge/gui/views/audio_clip_view.cpp
    VERBATIM
)
add_executable(LoopStationFinalTargetStackProbe "${FINAL_TARGET_STACK_PROBE}")
set_target_properties(LoopStationFinalTargetStackProbe PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopStationFinalTargetStackProbe COMMAND LoopStationFinalTargetStackProbe)
set(FINAL_PHYSICAL_BUTTON_PROBE "${CMAKE_CURRENT_BINARY_DIR}/generated/final_physical_button_probe.cpp")
add_custom_command(
    OUTPUT "${FINAL_PHYSICAL_BUTTON_PROBE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/generate_physical_button_probe.py"
            --buttons "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/hid/buttons.cpp"
            --view "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/views/view.cpp"
            --settings "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/menu_item/loop_station/settings.cpp"
            --support "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/physical_button_probe_support.h"
            --tests "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/physical_button_probe_tests.cpp"
            --output "${FINAL_PHYSICAL_BUTTON_PROBE}"
    DEPENDS final_fix/generate_physical_button_probe.py final_fix/physical_button_probe_support.h
            final_fix/physical_button_probe_tests.cpp ../../src/deluge/hid/buttons.cpp
            ../../src/deluge/gui/views/view.cpp ../../src/deluge/gui/menu_item/loop_station/settings.cpp
    VERBATIM
)
add_executable(LoopStationFinalPhysicalButtonProbe "${FINAL_PHYSICAL_BUTTON_PROBE}")
set_target_properties(LoopStationFinalPhysicalButtonProbe PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopStationFinalPhysicalButtonProbe COMMAND LoopStationFinalPhysicalButtonProbe)
set(FINAL_MODAL_HANDOFF_PROBE "${CMAKE_CURRENT_BINARY_DIR}/generated/final_modal_handoff_probe.cpp")
add_custom_command(
    OUTPUT "${FINAL_MODAL_HANDOFF_PROBE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/generate_modal_handoff_probe.py"
            --matrix "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/hid/matrix/matrix_driver.cpp"
            --load-ui "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/ui/load/load_song_ui.cpp"
            --support "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/modal_handoff_probe_support.h"
            --tests "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/modal_handoff_probe_tests.cpp"
            --output "${FINAL_MODAL_HANDOFF_PROBE}"
    DEPENDS final_fix/generate_modal_handoff_probe.py final_fix/modal_handoff_probe_support.h
            final_fix/modal_handoff_probe_tests.cpp ../../src/deluge/hid/matrix/matrix_driver.cpp
            ../../src/deluge/gui/ui/load/load_song_ui.cpp
    VERBATIM
)
add_executable(LoopStationFinalModalHandoffProbe "${FINAL_MODAL_HANDOFF_PROBE}")
set_target_properties(LoopStationFinalModalHandoffProbe PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopStationFinalModalHandoffProbe COMMAND LoopStationFinalModalHandoffProbe)
set(FINAL_SAVE_LOAD_HANDOFF_PROBE "${CMAKE_CURRENT_BINARY_DIR}/generated/final_save_load_handoff_probe.cpp")
add_custom_command(
    OUTPUT "${FINAL_SAVE_LOAD_HANDOFF_PROBE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/generate_save_load_handoff_probe.py"
            --view "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/views/view.cpp"
            --loop-view "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/views/loop_station_view.cpp"
            --ui "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/ui/ui.cpp"
            --matrix "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/hid/matrix/matrix_driver.cpp"
            --load-ui "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/ui/load/load_song_ui.cpp"
            --support "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/save_load_handoff_probe_support.h"
            --tests "${CMAKE_CURRENT_SOURCE_DIR}/final_fix/save_load_handoff_probe_tests.cpp"
            --output "${FINAL_SAVE_LOAD_HANDOFF_PROBE}"
    DEPENDS final_fix/generate_save_load_handoff_probe.py final_fix/save_load_handoff_probe_support.h
            final_fix/save_load_handoff_probe_tests.cpp ../../src/deluge/gui/views/view.cpp
            ../../src/deluge/gui/views/loop_station_view.cpp ../../src/deluge/gui/ui/ui.cpp
            ../../src/deluge/hid/matrix/matrix_driver.cpp ../../src/deluge/gui/ui/load/load_song_ui.cpp
    VERBATIM
)
add_executable(LoopStationFinalSaveLoadHandoffProbe "${FINAL_SAVE_LOAD_HANDOFF_PROBE}")
set_target_properties(LoopStationFinalSaveLoadHandoffProbe PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopStationFinalSaveLoadHandoffProbe COMMAND LoopStationFinalSaveLoadHandoffProbe)
set(FOREGROUND_SERVICE_PROBE "${CMAKE_CURRENT_BINARY_DIR}/generated/foreground_service_probe.cpp")
add_custom_command(
    OUTPUT "${FOREGROUND_SERVICE_PROBE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/navigation/generate_foreground_service_probe.py"
            --firmware "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/loop_station/song_model_firmware.cpp"
            --support "${CMAKE_CURRENT_SOURCE_DIR}/navigation/foreground_service_probe_support.h"
            --tests "${CMAKE_CURRENT_SOURCE_DIR}/navigation/foreground_service_probe_tests.cpp"
            --output "${FOREGROUND_SERVICE_PROBE}"
    DEPENDS navigation/generate_foreground_service_probe.py navigation/foreground_service_probe_support.h
            navigation/foreground_service_probe_tests.cpp
            ../../src/deluge/loop_station/song_model_firmware.cpp
    VERBATIM
)
add_executable(LoopStationForegroundServiceProbe "${FOREGROUND_SERVICE_PROBE}")
set_target_properties(LoopStationForegroundServiceProbe PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopStationForegroundServiceProbe COMMAND LoopStationForegroundServiceProbe)
add_custom_command(
    OUTPUT "${LOOP_DISPATCH_PROBE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui/generate_dispatch_probe.py"
            --ui "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/ui/ui.cpp"
            --timers "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/ui_timer_manager.cpp"
            --buttons "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/hid/buttons.cpp"
            --output "${LOOP_DISPATCH_PROBE}"
    DEPENDS ui/generate_dispatch_probe.py ui/probe_support.h
            ../../src/deluge/gui/ui/ui.cpp ../../src/deluge/gui/ui_timer_manager.cpp
            ../../src/deluge/hid/buttons.cpp
    VERBATIM
)
set(ENCODER_PROBE "${CMAKE_CURRENT_BINARY_DIR}/generated/production_encoder.cpp")
add_custom_command(
    OUTPUT "${ENCODER_PROBE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui/generate_encoder_probe.py"
            --encoder "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/hid/encoder_input.cpp"
            --support "${CMAKE_CURRENT_SOURCE_DIR}/ui/encoder_probe_support.h"
            --tests "${CMAKE_CURRENT_SOURCE_DIR}/ui/encoder_probe_tests.cpp"
            --output "${ENCODER_PROBE}"
    DEPENDS ui/generate_encoder_probe.py ui/encoder_probe_support.h ui/encoder_probe_tests.cpp
            ../../src/deluge/hid/encoder_input.cpp
    VERBATIM
)
add_executable(LoopStationEncoderProbe "${ENCODER_PROBE}")
set_target_properties(LoopStationEncoderProbe PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopStationEncoderProbe COMMAND LoopStationEncoderProbe)
set(SEVEN_SEGMENT_PROBE "${CMAKE_CURRENT_BINARY_DIR}/generated/production_seven_segment.cpp")
add_custom_command(
    OUTPUT "${SEVEN_SEGMENT_PROBE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui/generate_seven_segment_probe.py"
            --seven "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/hid/display/seven_segment.cpp"
            --numeric-layer "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/hid/display/numeric_layer/numeric_layer.cpp"
            --basic-layer "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/hid/display/numeric_layer/numeric_layer_basic_text.cpp"
            --scrolling-layer "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/hid/display/numeric_layer/numeric_layer_scrolling_text.cpp"
            --support "${CMAKE_CURRENT_SOURCE_DIR}/ui/seven_segment_probe_support.h"
            --tests "${CMAKE_CURRENT_SOURCE_DIR}/ui/seven_segment_probe_tests.cpp"
            --output "${SEVEN_SEGMENT_PROBE}"
    DEPENDS ui/generate_seven_segment_probe.py ui/seven_segment_probe_support.h ui/seven_segment_probe_tests.cpp
            ../../src/deluge/hid/display/seven_segment.cpp
            ../../src/deluge/hid/display/numeric_layer/numeric_layer.cpp
            ../../src/deluge/hid/display/numeric_layer/numeric_layer_basic_text.cpp
            ../../src/deluge/hid/display/numeric_layer/numeric_layer_scrolling_text.cpp
    VERBATIM
)
add_executable(LoopStationSevenSegmentProbe "${SEVEN_SEGMENT_PROBE}")
set_target_properties(LoopStationSevenSegmentProbe PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopStationSevenSegmentProbe COMMAND LoopStationSevenSegmentProbe)
add_custom_command(
    OUTPUT "${LOOP_UI_PROBE}" "${CMAKE_CURRENT_BINARY_DIR}/generated/shared_menu.cpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/shared_menu_declarations.h" "${CMAKE_CURRENT_BINARY_DIR}/generated/settings_items.cpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/settings_items_declarations.h" "${CMAKE_CURRENT_BINARY_DIR}/generated/horizontal_settings.cpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/horizontal_settings_declarations.h" "${CMAKE_CURRENT_BINARY_DIR}/generated/strings_declarations.h" "${CMAKE_CURRENT_BINARY_DIR}/generated/english_cases.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui/generate_probe.py"
            --loop-view "${LOOP_UI_VIEW}" --settings-view "${LOOP_UI_SETTINGS}"
            --support "${CMAKE_CURRENT_SOURCE_DIR}/ui/probe_support.h" --output "${LOOP_UI_PROBE}"
    DEPENDS ui/generate_probe.py ui/probe_support.h "${LOOP_UI_VIEW}" "${LOOP_UI_SETTINGS}"
            ../../src/deluge/gui/l10n/english.json ../../src/deluge/gui/l10n/strings.h
            ui/settings_probe_support.h
            ../../src/deluge/gui/menu_item/loop_station/horizontal_settings.h
            ../../src/deluge/gui/menu_item/loop_station/horizontal_settings.cpp
            ../../src/deluge/gui/menu_item/submenu.h ../../src/deluge/gui/menu_item/submenu.cpp
            ../../src/deluge/gui/menu_item/horizontal_menu.h ../../src/deluge/gui/menu_item/horizontal_menu.cpp
            ../../src/deluge/gui/menu_item/decimal.cpp
            ../../src/deluge/gui/menu_item/selection.cpp ../../src/deluge/gui/menu_item/enumeration.cpp
            ../../src/deluge/gui/menu_item/number.cpp
            ../../src/deluge/gui/menu_item/menu_item.cpp ../../src/deluge/gui/menu_item/value.h
            ../../src/deluge/gui/menu_item/loop_station/settings_items.h
            ../../src/deluge/gui/menu_item/loop_station/settings_items.cpp
    VERBATIM
)
add_custom_command(
    OUTPUT "${STUTTER_LIFECYCLE_PROBE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui/generate_stutter_lifecycle_probe.py"
            --stutterer "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/model/fx/stutterer.cpp"
            --audio "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/model/mod_controllable/mod_controllable_audio.cpp"
            --view "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/views/view.cpp"
            --support "${CMAKE_CURRENT_SOURCE_DIR}/ui/probe_support.h"
            --output "${STUTTER_LIFECYCLE_PROBE}"
    DEPENDS ui/generate_stutter_lifecycle_probe.py ui/probe_support.h ui/stutter_lifecycle_support.h
            ../../src/deluge/model/fx/stutterer.cpp
            ../../src/deluge/model/mod_controllable/mod_controllable_audio.cpp
            ../../src/deluge/gui/views/view.cpp
    VERBATIM
)

add_executable(LoopStationTests
    "${CMAKE_CURRENT_BINARY_DIR}/generated/real_canvas.cpp"
    ../../src/deluge/gui/fonts/fonts.c
    "${LOOP_UI_PROBE}"
    "${STUTTER_LIFECYCLE_PROBE}"
    "${LOOP_DISPATCH_PROBE}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/horizontal_settings.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/shared_menu.cpp"
    ui/shared_menu_tests.cpp
    ui/settings_items_tests.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/settings_items.cpp"
    ui/probe_support.cpp
    ui/input_tests.cpp
    ui/horizontal_settings_tests.cpp
    ui/display_tests.cpp
    main.cpp
    view_tests.cpp
    pad_render_tests.cpp
    menu_tests.cpp
    ../../src/deluge/loop_station/menu_model.cpp
    ../../src/deluge/loop_station/geometry.cpp
    archive_tests.cpp
    transpose_tests.cpp
    ../../src/deluge/loop_station/transpose.cpp
    mocks/storage.cpp
    ../../src/deluge/loop_station/archive_storage.cpp
    ../../src/deluge/loop_station/archive.cpp
    song_model_tests.cpp
    lifecycle_tests.cpp
    ../../src/deluge/loop_station/song_model.cpp
    audio_adapter_tests.cpp
    ../../src/deluge/loop_station/audio_adapter.cpp
    ../../src/deluge/loop_station/audio_adapter_firmware.cpp
    ../../src/deluge/loop_station/transpose.cpp
    memory_reservation_tests.cpp
    ../hardware/loop_station/fixture.cpp
    ../../src/deluge/loop_station/diagnostics.cpp
    ../../src/deluge/loop_station/memory_reservation.cpp
    timing_tests.cpp
    length_display_tests.cpp
    engine_tests.cpp
    ../../src/deluge/loop_station/timing.cpp
    ../../src/deluge/loop_station/engine.cpp
    settings_tests.cpp
    page_pool_tests.cpp
    resident_audio_tests.cpp
    history_tests.cpp
    restore_tests.cpp
    persistence_tests.cpp
    ../../src/deluge/loop_station/persistence_firmware.cpp
    snapshot_tests.cpp
    gesture_rollback_tests.cpp
    ../../src/deluge/loop_station/history.cpp
    ../../src/deluge/loop_station/snapshot.cpp
    ../../src/deluge/loop_station/gesture_rollback.cpp
    ../../src/deluge/loop_station/resident_audio.cpp
    ../../src/deluge/loop_station/page_pool.cpp
    ../../src/deluge/loop_station/persistence.cpp
)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/real_canvas.cpp"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../oled_canvas/generate_shared_probe.py"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/real_canvas.cpp"
    DEPENDS ../oled_canvas/generate_shared_probe.py ../oled_canvas/generate_probe.py
            ui/real_canvas.h ui/shared_probe_support.h
            ../../src/deluge/hid/display/oled_canvas/canvas.cpp
            ../../src/deluge/hid/display/oled_canvas/canvas.h
            ../../src/deluge/gui/fonts/fonts.h
    VERBATIM
)
target_include_directories(LoopStationTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ui")
target_include_directories(LoopStationTests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" ../../src ../../src/deluge ../../build/src/deluge)
target_compile_definitions(LoopStationTests PRIVATE CPPUTEST_MEM_LEAK_DETECTION_DISABLED LOOP_STATION_TEST_MODE=1 LOOP_STATION_HOST_TEST=1)
target_link_libraries(LoopStationTests PRIVATE CppUTest etl::etl)
set_target_properties(LoopStationTests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopStationTests COMMAND LoopStationTests)
add_test(NAME LoopStationDemoContract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui/demo_contract_test.py")

add_executable(LoopPageAllocationGuard page_pool_allocation_guard.cpp ../../src/deluge/loop_station/page_pool.cpp)
target_include_directories(LoopPageAllocationGuard PRIVATE ../../src)
set_target_properties(LoopPageAllocationGuard PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopPageAllocationGuard COMMAND LoopPageAllocationGuard)

option(LOOP_STATION_UBSAN "Instrument loop station host tests with UBSan" OFF)
if(LOOP_STATION_UBSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(LoopStationTests PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
    target_link_options(LoopStationTests PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
endif()

add_executable(LoopAudioAllocationGuard resident_audio_allocation_guard.cpp
    ../../src/deluge/loop_station/audio_adapter.cpp ../../src/deluge/loop_station/transpose.cpp
    ../../src/deluge/loop_station/engine.cpp
    ../../src/deluge/loop_station/timing.cpp
    ../../src/deluge/loop_station/page_pool.cpp ../../src/deluge/loop_station/resident_audio.cpp
    ../../src/deluge/loop_station/history.cpp
    ../../src/deluge/loop_station/snapshot.cpp
    ../../src/deluge/loop_station/gesture_rollback.cpp)
target_include_directories(LoopAudioAllocationGuard PRIVATE ../../src)
set_target_properties(LoopAudioAllocationGuard PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopAudioAllocationGuard COMMAND LoopAudioAllocationGuard)

add_executable(LoopGestureAllocationGuard gesture_rollback_allocation_guard.cpp
    ../../src/deluge/loop_station/page_pool.cpp ../../src/deluge/loop_station/resident_audio.cpp
    ../../src/deluge/loop_station/history.cpp
    ../../src/deluge/loop_station/snapshot.cpp
    ../../src/deluge/loop_station/gesture_rollback.cpp)
target_include_directories(LoopGestureAllocationGuard PRIVATE ../../src)
set_target_properties(LoopGestureAllocationGuard PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopGestureAllocationGuard COMMAND LoopGestureAllocationGuard)
if(LOOP_STATION_UBSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(LoopGestureAllocationGuard PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
    target_link_options(LoopGestureAllocationGuard PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
endif()

add_executable(LoopHistorySnapshotAllocationGuard history_snapshot_allocation_guard.cpp
    ../../src/deluge/loop_station/page_pool.cpp ../../src/deluge/loop_station/resident_audio.cpp
    ../../src/deluge/loop_station/gesture_rollback.cpp ../../src/deluge/loop_station/history.cpp
    ../../src/deluge/loop_station/snapshot.cpp)
target_include_directories(LoopHistorySnapshotAllocationGuard PRIVATE ../../src)
set_target_properties(LoopHistorySnapshotAllocationGuard PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_test(NAME LoopHistorySnapshotAllocationGuard COMMAND LoopHistorySnapshotAllocationGuard)
if(LOOP_STATION_UBSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(LoopHistorySnapshotAllocationGuard PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
    target_link_options(LoopHistorySnapshotAllocationGuard PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
endif()

add_executable(LoopFatFsAllocationTests main.cpp fatfs_archive_tests.cpp ../../src/fatfs/ff.c ../../src/fatfs/ffunicode.c)
target_include_directories(LoopFatFsAllocationTests PRIVATE ../../src ../../src/fatfs)
target_compile_definitions(LoopFatFsAllocationTests PRIVATE CPPUTEST_MEM_LEAK_DETECTION_DISABLED)
target_link_libraries(LoopFatFsAllocationTests PRIVATE CppUTest)
set_target_properties(LoopFatFsAllocationTests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON C_STANDARD 23)
add_test(NAME LoopFatFsAllocationTests COMMAND LoopFatFsAllocationTests)
if(LOOP_STATION_UBSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(LoopFatFsAllocationTests PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
    target_link_options(LoopFatFsAllocationTests PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
endif()
