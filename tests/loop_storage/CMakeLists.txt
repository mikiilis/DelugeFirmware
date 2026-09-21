add_executable(LoopStorageTests
    ../unit/RunAllTests.cpp
    storage_tests.cpp
    ../../src/fatfs/ff.c
    ../../src/fatfs/ffunicode.c
    ../../src/fatfs/fatfs.cpp)
target_include_directories(LoopStorageTests PRIVATE ../../src ../../src/deluge)
target_compile_features(LoopStorageTests PRIVATE cxx_std_23)
target_compile_definitions(LoopStorageTests PRIVATE CPPUTEST_MEM_LEAK_DETECTION_DISABLED)
target_link_libraries(LoopStorageTests PRIVATE CppUTestExt)
if(RECORDER_TEST_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(LoopStorageTests PRIVATE -fsanitize=${RECORDER_TEST_SANITIZER_SET} -fno-omit-frame-pointer)
    # ff.c deliberately uses unaligned little-endian accesses on supported targets.
    set_source_files_properties(../../src/fatfs/ff.c PROPERTIES COMPILE_OPTIONS "-fno-sanitize=alignment")
    target_link_options(LoopStorageTests PRIVATE -fsanitize=${RECORDER_TEST_SANITIZER_SET})
endif()
add_test(NAME LoopStorageTests COMMAND LoopStorageTests)
