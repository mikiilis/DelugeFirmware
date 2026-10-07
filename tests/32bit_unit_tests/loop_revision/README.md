# Loop revision ownership tests

Run on Linux with the repository's 32-bit/multilib test configuration:

```sh
cmake --build build/tests --target LoopRevisionOwnershipTests
ctest --test-dir build/tests -R '^LoopRevisionOwnershipTests$' --output-on-failure
```

Use `--config Debug` / `-C Debug` for multi-config builds.

The target compiles production revision history, String, external_allocator and
GeneralMemoryAllocator/MemoryRegion. `MEMORY_ALLOCATOR_TEST_REGIONS` skips the
hardware-address constructor setup and allows the fixture to install host arenas;
allocation and deallocation still use production code. AudioClip, Sample reason
counters and unavailable-file loading are boundaries replaced by local mocks.

Coverage: 101 delayed publications, final path rename after staging, direct
`publishGeneration()` ownership of the final Sample path through the real
32-bit String and allocator, vector reallocation, rollback retaining redo,
branch replacement, copy/move and self-assignment, and cloning a String in the
general external arena. This target
requires 32-bit pointers: a native 64-bit String fixture would corrupt its
four-byte reference-count header and is not a substitute.

The suite has not been executed locally on macOS. ARM 32-bit syntax checks cover
the new test, revision implementation, String and allocator test configuration;
Linux runtime verification remains required.
