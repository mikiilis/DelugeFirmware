# Loop station feasibility fixture (hardware unqualified)

The Loop Station product view and settings menus are implemented, while the
physical feasibility gate remains **NOT PASSED**. The user has a device but
explicitly deferred hardware/debug-log work and asked to continue software
implementation. No hardware acceptance, display inspection, memory measurement,
or audio-output measurement was performed. `acceptance.csv` retains the full
OLED/seven-segment × Preferred/TenSecond matrix; every physical result and
measurement is `NOT_RUN`. A separate `host_evidence` column records relevant
software tests without treating them as hardware acceptance. Byte columns are
unqualified accounting candidates, not measured allocation or free-memory
results. No minimum free-memory threshold or supported hardware load has been
qualified. Preferred remains configured; implementation and host tests do not
qualify memory fit, callback deadlines, input latency, or output quality.

Software verification recorded for this implementation update (host evidence
only): `DBT_NO_SYNC=1 ./dbt test` passed 17/17 CTest targets, including all
allocation guards; `LoopStationTests` passed 286 tests / 3,564,913 checks; the
selected UBSan CTest run passed 5/5 targets; and `DBT_NO_SYNC=1 ./dbt build
debug` linked the Debug ELF (text/data/bss 1,882,256 / 70,776 / 411,828 bytes).
The new mixed-length host case is listed per acceptance row. None of these
results is a physical pass.

The website build was attempted but could not run through Astro: `tsx` is not
installed in the website package, and `pnpm install --offline` could not find
the `@astrojs/starlight@0.32.6` tarball in the local store. No network install
was attempted. The generated website JSON prebuild outputs were restored after
the failed attempt.

This is a developer workload for Task 8, distinct from the implemented product
feature. The runtime **Enable Loop Station** community feature defaults **Off**.
The optional `LOOP_STATION_TEST_MODE` harness also defaults **Off**. CMake
includes its fixture and audio-engine hooks only for an explicitly enabled
**Debug** build; Release and RelWithDebInfo exclude them even when the option is
ON. A non-debug manual compile with the macro enabled fails.

```
DBT_NO_SYNC=1 ./dbt configure -DLOOP_STATION_TEST_MODE=ON
DBT_NO_SYNC=1 ./dbt build debug
```

`DBT_NO_SYNC=1` avoids repository synchronization for the linked worktree; it does not skip compiler checks. Restore the harness default with `DBT_NO_SYNC=1 ./dbt configure -DLOOP_STATION_TEST_MODE=OFF` and rebuild. This does not change the separate default-Off product runtime feature flag.

AudioEngine::init initializes the fixture after the allocator exists, in the foreground. All reservation, teardown, debugger actions, diagnostics sampling, render/control and optional archive servicing must be serialized on the foreground owner. Never call acquire/release or fixture lifecycle from an interrupt, audio render, or concurrently with callbacks. The normal audio render path invokes the preinitialized fixture with deterministic stereo input `{4096,-4096}`. The fixture routes every slot through `AudioAdapter::finalizeWindow()` and `AudioAdapter::renderDry()`, so it exercises per-track transpose after backing/monitor combination. Its output is discarded; it measures DSP work alongside the current song without changing product audio or adding UI. It does not validate analog input quality or provide captured loop output for pitch/continuity review.

The boot request is explicitly `start(kConfiguredMemoryProfile)` (Preferred remains configured). It records ten seconds on all eight slots using one common manual page-grant round. At the exact phrase boundary all eight become repeating playback tracks; capture toggles on all tracks every 44,100 frames. To test TenSecond explicitly, pause callback execution in the debugger, call `firmwareFixture->stop()`, then `firmwareFixture->start(MemoryProfile::TenSecond)`. To stress the maximum per-track preferred length, pass `524288` as the second start argument. No failed request retries with a smaller profile. `toggleCapture()` is also available on the same serialized owner.

For eight-track transpose timing, pause callback execution and call
`firmwareFixture->setTransposeCents(0)`, resume and record unity cost; pause
again and repeat at `2450` and `-2450` before resuming. This updates all eight
slot ratios at once. Keep the card workload/archive choice and declared Song,
voice, output and FX load identical across runs. Capture the complete audio
callback PMU maximum and total, then compare the maximum with the real callback
deadline. The fixture continues to discard its output, so pitch quality,
discontinuities and captured-audio continuity still require real loopstation
output capture on the device. Every such result remains `NOT_RUN` until the
physical matrix is performed.

## Storage ownership and accounting

Every arena is separately allocated by GeneralMemoryAllocator::allocLowSpeed, which uses `mayUseOnChipRam=false, makeStealable=false`. Large requests try the general external allocator and then the large stealable *region* with non-stealable allocation headers. A region's name does not make these allocations stealable. The 128 KiB small external allocator is never used for these requests, and global allocator partitioning is unchanged. Firmware checks each returned actual address against SDRAM `0x0c000000..0x10000000`, permits only general external or stealable-region allocations, checks the non-stealable header, and prints half-open address ranges, exact assigned bytes, allocator region and `pinned=1`. A failure releases the partial reservation and reports failure.

| Arena | Preferred bytes | TenSecond bytes |
|---|---:|---:|
| Active audio | 33,554,432 | 28,246,016 |
| History candidate | 4,194,304 | 4,194,304 |
| Snapshot before-images candidate | 4,194,304 | 4,194,304 |
| Gesture rollback | `GestureRollback::requiredBytes()` (3,704,448 on host and target compiler) | same |
| Fixed snapshot read staging | 4,096 | 4,096 |
| Arena subtotal using measured target ABI | 45,651,584 | 40,343,168 |

These candidates are **unqualified**. Target rollback size is derived by the target compiler and printed at boot. Fixed observer/engine/fixture object metadata is placed in `.sdram_bss`; the target compiler measures `sizeof(Fixture)` at boot (the Task 8 baseline was 39,016 bytes before the transpose adapter was added). The updated target size must be recorded from the rebuilt ELF before physical testing. The fixture's `Transpose` contains 262,144 bytes of eight-slot history and 512 bytes of cursor/head state; its host `sizeof` is 262,656 bytes. This fixture object increment is separate from the product `FirmwareOwner` increment and must be counted once for each allocation under measurement. The fixed target diagnostic snapshot is 256 bytes. PagePool additionally owns two startup-only `new[]` metadata allocations: 294,912 bytes Preferred or 248,256 bytes TenSecond, excluding allocator headers/rounding. Their existing allocator policy may use internal RAM; hardware tests must measure that cost. The original Task 8 candidate totals of 45,985,512 bytes Preferred or 40,630,440 bytes TenSecond predate this state and are stale for the updated fixture; replace the fixed fixture-size term from the target build before use. All totals still exclude allocator overhead and other firmware consumers. No per-render allocation is introduced. The arena subtotal excludes fixed object metadata, PagePool metadata, allocator headers/alignment/fragmentation, firmware/song/voices/clusters, stack, and hardware margins. Map evidence establishes static placement, not successful dynamic reservation or hardware fit.

`MemoryReservation::pin()` is a coarse lifetime pin covering **all** PagePool phrases, readers, gesture entries, snapshots and staging consumers within a session. The session owns that pin until every consumer is destroyed. Callers must retain a pin whenever a phrase/reader can outlive a release request; raw spans cannot track their own lifetime. Release with live pins is deferred, new pins and profile replacement are rejected, and the last foreground pin releases all five allocations once. The reservation and allocator must outlive pins. There is no background thread, blocking wait, page theft, global allocator override, or allocator mutation in render.

## Diagnostics and archive servicing

On the serialized owner inspect `firmwareFixture->diagnostics().snapshot()`. It returns a fixed value with the requested profile, five last-request assigned byte counts and addresses, arena subtotal, callback count/total/max PMU cycles, wall frames, per-track captured/playback frame totals, capture/playback masks, cumulative successfully copied before-image page/frame counts, history pressure events, current history/snapshot pages, storage invocation count, and status. After stop, the last run's counters/layout remain available with status Unavailable; those addresses must not be dereferenced. Failed reservation attempts report zero assigned bytes and the requested profile. Page-copy totals survive eviction. Pressure counts history arena-exhaustion encounters or descriptor-exhaustion encounters; it is not an SD latency measure. PMU cycles cover the fixture callback, including its bookkeeping; compare the **whole normal audio callback** separately against its deadline. Host tests inject a deterministic counter and cannot establish timing.

Archive service starts disabled and may stay disabled indefinitely. The firmware fixture supplies **no SD writer**, so the default has zero storage calls. A developer can inject an `ArchiveSink` when constructing a fixture, enable it, and call `serviceArchive()` explicitly from a safe foreground service opportunity. This issues at most one 4096-byte staged snapshot block per track per service call, checks validity again after a yielding sink, and releases its snapshot; it is a benchmark adapter, not a finished archive or publish protocol. It publishes no file and never runs from the audio callback. Staging retains a separate lifetime pin across sink yields, including fixture disable. The writer may not retain a staging span after returning.

## Qualification procedure

Record board/firmware commit/build flags, map, boot allocation ranges and totals for **each explicitly requested profile** in `acceptance.csv`. Exercise eight loops with capture on/off and with archive service disabled throughout; repeat under maximum expected song/voice/cluster load and long enough to cross history capacity. Run identical eight-track unity, +2,450-cent and -2,450-cent passes through the now-integrated `AudioAdapter` transpose stage. Inspect callback maximum/total cycles, copied pages/frames, history pressure, live frame totals, audio glitches and load failures. Record audio output at unity and both extremes to verify pitch, warm-up/restart continuity and dry saved captures. Then test a real yielding SD sink separately, including failed/slow storage and snapshot cancellation. Compare full callback cost with the sample deadline, not just the fixture's isolated counter. Repeat enable/disable under allocator pressure and verify the heap returns to its baseline.

Do not mark an acceptance row passed from host tests, static map placement, synthetic input, or successful compilation. Actual hardware allocation ranges, allocator pressure, usable loop length, uninterrupted output, target gesture size, worst-case copy cost and full-system margins remain Task 8 work.
