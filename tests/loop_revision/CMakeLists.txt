# Exercise the production revision transaction on native hosts. The separate
# 32-bit target additionally validates firmware String/allocator ownership.
add_executable(LoopRevisionPublicationTests
    ../unit/RunAllTests.cpp
    publication_tests.cpp
    string_fixture.cpp
    ../../src/deluge/loop_station/revision.cpp)
target_include_directories(LoopRevisionPublicationTests PRIVATE
    mocks ../32bit_unit_tests/loop_revision/mocks ../../src ../../src/deluge)
target_compile_features(LoopRevisionPublicationTests PRIVATE cxx_std_23)
target_compile_definitions(LoopRevisionPublicationTests PRIVATE IN_UNIT_TESTS=1 CPPUTEST_MEM_LEAK_DETECTION_DISABLED)
target_link_libraries(LoopRevisionPublicationTests PRIVATE CppUTestExt etl::etl)
if(RECORDER_TEST_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(LoopRevisionPublicationTests PRIVATE -fsanitize=${RECORDER_TEST_SANITIZER_SET} -fno-omit-frame-pointer)
    target_link_options(LoopRevisionPublicationTests PRIVATE -fsanitize=${RECORDER_TEST_SANITIZER_SET})
endif()
add_test(NAME LoopRevisionPublicationTests COMMAND LoopRevisionPublicationTests)
