# E036: rapid short-loop finalization

## Finding

E036 is the diagnostic in `SampleCluster::~SampleCluster()` for destroying a
cluster with outstanding ownership reasons (excluding the existing special
case for the active loader). It is not a four-loop resource-limit error.

Audio Clip recorders deliberately retain the first two written clusters for
immediate playback. Smart-stop cropping can remove the second cluster before
the recorder is destroyed. Previously, `truncateFileDownToSize()` destructed
that cluster without releasing the recorder's retained reason. An isolated
probe using the unchanged production truncation/destructor bodies produced
E036 with a retained reason and succeeded with no reasons. The exact reported
hardware gesture has not been reproduced here.

The earlier lifecycle tests could not catch this path: they stub finalization.

## Implementation

- Written startup-cluster ownership is now an explicit private bitmask. Both
  completed-block writing and final partial-block writing record retained
  ownership. Trimming and ordinary/abort cleanup clear each reason once.
- Trimming validates its size and completed-writer state, releases only the
  recorder's discarded startup reasons, and checks the entire discarded tail
  for remaining recorder, playback, or loading ownership.
- The file is sought/truncated before descriptors are removed. Ownership is
  rechecked after storage yields, and cluster pointers are fetched again after
  possible recycling. Storage failure leaves descriptors available for cleanup.
- Unexpected remaining ownership returns `Error::BUG` through the existing
  commit rollback. Loop Station maps it to a runtime-only internal error instead
  of misreporting an SD-card failure. E036 remains enabled as a diagnostic.
- Missing header-cluster reload now reports a storage failure instead of
  silently publishing a recording whose header was not updated.
- Finalization and cluster destruction live in separate production translation
  units so regression tests can execute the real implementations.
- RAM-only diagnostics include slot/source, trim sizes, writer indices, startup
  ownership, and cluster reasons. An E036 failure snapshot remains available
  even after older recorder events have rolled out of the ring.

No song XML format, persistent settings, loop duration restriction, playback
delay, recorder-capacity limit, or global reference-count reset was introduced.

## Verification boundaries

See `tests/recorder_lifecycle/README.md` for commands and the hardware procedure.
The new tests exercise real finalization, trimming, header updates, and cluster
destruction, with resource and physical-storage boundaries injected. They cover
retained blocks, reference release/recycling, mono/stereo/margins, boundary
lengths, finalization failures, abort, and 4,000 short-loop finalizations in groups
of four. Engine tests separately cover commit rollback and history preservation.

The tests do not emulate physical SD/FAT behavior, audio rendering, the real
memory-stealing allocator, or full view/clip/history integration. Hardware
stress and the exact reported four-loop sequence remain outstanding. Nothing
has been flashed. ASan could not initialize on this Mac in the preceding work;
UBSan is the available runnable sanitizer for the recorder targets.

Validated host results:

- All nine configured CTest targets pass.
- UnitTests: 227 tests, 307,972 checks.
- RecorderLifecycleTests: 17 tests, 26,144 checks.
- RecorderFinalizationTests: 15 tests, 20,251 checks, including 4,000 successful
  finalizations. Both recorder targets pass with UBSan configured to halt on
  the first error.
- `git diff --check` passes.

## Firmware artifacts

Both final Debug and Release builds pass with version
`v1.3.0-dev-f8ae1425-dirty`. The existing RWX LOAD-segment linker warning remains;
Debug also reports the existing startup-song stack-usage warning. Both ELF files
retain the recorder trace and E036 failure snapshot symbols, including under
Release LTO.

- Release: `build/Release/deluge.bin`, 1,766,212 bytes.
  SHA-256: `1641d328e4e768cdd8b9bd6058b6fc04b29fa443a036cf1f35966b7a08a8de58`.
- Debug: `build/Debug/deluge.bin`, 1,822,888 bytes.
  SHA-256: `b4963f6ba83b51d7f031baad2d7ec556d4c664de8369c5b1797c7659e485b101`.
