# Compile the production recorder ownership/cleanup implementation, with only its
# hardware/resource dependencies replaced. Keep these mocks out of other tests.
add_executable(RecorderLifecycleTests
    ../unit/RunAllTests.cpp
    recorder_lifecycle_tests.cpp
    recorder_loop_tests.cpp
    fixture.cpp
    ../../src/deluge/model/sample/sample_recorder_lifecycle.cpp
    ../../src/deluge/model/sample/sample_recorder_card.cpp
    ../../src/deluge/model/sample/sample_recorder_loop.cpp
    ../../src/deluge/model/sample/sample_cluster_lifecycle.cpp
)
target_include_directories(RecorderLifecycleTests PRIVATE mocks . ../../src ../../src/deluge)
target_compile_features(RecorderLifecycleTests PRIVATE cxx_std_23)
target_compile_definitions(RecorderLifecycleTests PRIVATE IN_UNIT_TESTS=1 CPPUTEST_MEM_LEAK_DETECTION_DISABLED)
target_link_libraries(RecorderLifecycleTests PRIVATE CppUTestExt etl::etl)
option(RECORDER_TEST_SANITIZERS "Instrument recorder lifecycle tests with ASan and UBSan" OFF)
set(RECORDER_TEST_SANITIZER_SET "address,undefined" CACHE STRING "Sanitizers for recorder lifecycle tests")
if(RECORDER_TEST_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(RecorderLifecycleTests PRIVATE -fsanitize=${RECORDER_TEST_SANITIZER_SET} -fno-omit-frame-pointer)
    target_link_options(RecorderLifecycleTests PRIVATE -fsanitize=${RECORDER_TEST_SANITIZER_SET})
endif()
add_test(NAME RecorderLifecycleTests COMMAND RecorderLifecycleTests)

# Unlike the lifecycle target, this compiles the actual writer bookkeeping,
# WAV finalization, truncation, and SampleCluster destructor.
add_executable(RecorderFinalizationTests
    ../unit/RunAllTests.cpp
    recorder_finalization_tests.cpp
    fixture.cpp
    ../../src/deluge/model/sample/sample_recorder_lifecycle.cpp
    ../../src/deluge/model/sample/sample_recorder_card.cpp
    ../../src/deluge/model/sample/sample_recorder_loop.cpp
    ../../src/deluge/model/sample/sample_recorder_finalization.cpp
    ../../src/deluge/model/sample/sample_cluster_lifecycle.cpp
)
target_include_directories(RecorderFinalizationTests PRIVATE mocks . ../../src ../../src/deluge)
target_compile_features(RecorderFinalizationTests PRIVATE cxx_std_23)
target_compile_definitions(RecorderFinalizationTests PRIVATE IN_UNIT_TESTS=1 RECORDER_FINALIZATION_TESTS=1 CPPUTEST_MEM_LEAK_DETECTION_DISABLED)
target_link_libraries(RecorderFinalizationTests PRIVATE CppUTestExt etl::etl)
if(RECORDER_TEST_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(RecorderFinalizationTests PRIVATE -fsanitize=${RECORDER_TEST_SANITIZER_SET} -fno-omit-frame-pointer)
    target_link_options(RecorderFinalizationTests PRIVATE -fsanitize=${RECORDER_TEST_SANITIZER_SET})
endif()
add_test(NAME RecorderFinalizationTests COMMAND RecorderFinalizationTests)
