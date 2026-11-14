find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(LOOP_UI_PROBE "${CMAKE_CURRENT_BINARY_DIR}/generated/production_ui.cpp")
set(LOOP_UI_VIEW "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/views/loop_station_view.cpp")
set(LOOP_UI_SETTINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../src/deluge/gui/menu_item/loop_station/settings.cpp")
add_custom_command(
    OUTPUT "${LOOP_UI_PROBE}" "${CMAKE_CURRENT_BINARY_DIR}/generated/shared_menu.cpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/shared_menu_declarations.h" "${CMAKE_CURRENT_BINARY_DIR}/generated/settings_items.cpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/settings_items_declarations.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui/generate_probe.py"
            --loop-view "${LOOP_UI_VIEW}" --settings-view "${LOOP_UI_SETTINGS}"
            --support "${CMAKE_CURRENT_SOURCE_DIR}/ui/probe_support.h" --output "${LOOP_UI_PROBE}"
    DEPENDS ui/generate_probe.py ui/probe_support.h "${LOOP_UI_VIEW}" "${LOOP_UI_SETTINGS}"
            ../../src/deluge/gui/menu_item/submenu.h ../../src/deluge/gui/menu_item/submenu.cpp
            ../../src/deluge/gui/menu_item/horizontal_menu.h ../../src/deluge/gui/menu_item/horizontal_menu.cpp
            ../../src/deluge/gui/menu_item/decimal.cpp
            ../../src/deluge/gui/menu_item/menu_item.cpp ../../src/deluge/gui/menu_item/value.h
            ../../src/deluge/gui/menu_item/loop_station/settings_items.h
            ../../src/deluge/gui/menu_item/loop_station/settings_items.cpp
    VERBATIM
)

add_executable(LoopStationTests
    "${LOOP_UI_PROBE}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/shared_menu.cpp"
    ui/shared_menu_tests.cpp
    ui/settings_items_tests.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/settings_items.cpp"
    ui/probe_support.cpp
    ui/input_tests.cpp
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
