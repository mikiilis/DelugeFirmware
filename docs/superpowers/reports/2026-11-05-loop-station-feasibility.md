# First loop-station feasibility gate

## Software status update — Task 15 (2026-11-10)

The loop playback transpose stage is now implemented and host signal tests cover
unity bypass, rounded Q24 ratios, +12-semitone output pitch, ratio edits during
an active grain crossfade, bypass/discontinuity reset, Monitoring Off playback,
dry capture, stale idle-monitor history on playback start, and one-shot retrigger
history reset while playback remains active. This is software evidence only;
**memory reserve, eight-track CPU cost, extreme-shift output/continuity and
hardware qualification are NOT_RUN**. The hardware gate remains **NOT PASSED**.

## Task 18 software and documentation status — 2026-11-10

The product implementation now includes the default-Off Loop Station runtime
feature, eight-track performance view, global and track settings, settings copy,
ordinary AudioClip access, shared-page duration limits and audio-safe routing.
The public guide is `website/src/content/docs/features/loop_station.mdx`.
This is implementation status, not device qualification.

Task 18's host regression audit found existing coverage for both Record Actions
(`LoopEngine::one_shot_record_actions_and_explicit_stop` and
`LoopAudioAdapter::capacitySplitsDeviceWindowAndImmediatelyPlaysOrDubsWithoutLosingTail`),
source/monitor priority (`capturingTracksOwnOverlappingPhysicalMonitorSources`),
late rapid taps (`eightLastSampleDoubleTapsBoundDispatchAndFairRetirement`),
unsaved edits during save (`statePreservesUnsavedEditsThatOccurAfterTheSaveSnapshot`),
and durable manifest restart (`simulatedRestartUsesOldManifestUntilVerifiedCommitThenNewManifest`).
A new `LoopAudioAdapter::eightTracksKeepUnequalShortAndLongLoopPhasesDuringSharedOverdubs`
regression exercises all eight unequal 256/513-frame phrases while overdubs run
through shared page crossings and repeated wraps. These are host tests; none
establish actual device callback timing, memory reserves or audio quality.

Software verification completed for this update:

- `DBT_NO_SYNC=1 ./dbt test` — 17/17 CTest targets passed, including the
  mixed-length regression and allocation guards.
- `./build/tests/loop_station/Debug/LoopStationTests` — 286 tests and
  3,564,913 checks passed.
- `cmake --build build/tests-loop-ubsan --config Debug --target LoopStationTests LoopAudioAllocationGuard LoopGestureAllocationGuard LoopHistorySnapshotAllocationGuard -j4`
  followed by `ctest --test-dir build/tests-loop-ubsan -C Debug -R 'LoopStationTests|Loop.*AllocationGuard' --output-on-failure`
  — 5/5 tests passed without sanitizer diagnostics.
- `DBT_NO_SYNC=1 ./dbt build debug` — Debug ELF linked; text/data/bss are
  1,882,256 / 70,776 / 411,828 bytes. Linker emitted the existing RWX LOAD
  segment warning.
- Website build is blocked by absent offline dependencies. `pnpm build` from
  `website/` ran its generated-data steps, then failed because `tsx` is absent;
  `pnpm install --offline` failed because `@astrojs/starlight@0.32.6` is not in
  the local package store. No network install was attempted. The generated
  recent-change/menu-hierarchy outputs were restored; the new MDX guide remains.

The physical matrix remains `NOT_RUN` by the user's explicit direction. No
OLED/seven-segment inspection, physical allocation, callback deadline,
DMA/input latency, card-removal, SD-stall or captured-output measurement was
performed. The former Task 8 statement to stop before Task 9 is historical;
software implementation has continued, while the physical acceptance gate
remains open.

The acceptance matrix adds a software-only `host_evidence` field. Its `result`
and measurement fields continue to describe physical acceptance and remain
`NOT_RUN`; host evidence does not convert any physical row to passed. The stale
Task 8 fixture object-size estimate is marked `NOT_RUN` in CSV because a current
runtime object measurement was not collected. No supported hardware capacity,
CPU load or song/FX envelope is claimed.

Each `SongModel` now owns eight fixed transpose histories: 4,096 stereo frames
per slot, or 262,144 bytes of sample history. Head/cursor state brings
`sizeof(Transpose)` to 262,656 bytes in the host ABI (the target link builds the
same fixed-width state). This is allocated with the externally allocated
`FirmwareOwner`, outside the five `MemoryReservation` arenas in the accounting
below. Add this 262,656-byte owner increment to future allocator/high-water
measurements; it does not establish that either profile has enough reserve.
Eight-track CPU timing at unity and +/-2,450 cents with declared song/FX load
remains **NOT_RUN**; do not infer support from host frequency checks or the
successful firmware link.

Recorded 2026-11-08 against `aea96030` (Task 7), with Task 8 host tests and documentation added. **Gate: NOT PASSED — hardware measurements unavailable.** At that gate run no compatible physical Deluge was available. The user later reported having a device, then asked to stop troubleshooting its test connection and continue implementation. No OLED or seven-segment device was booted, flashed, recorded or measured during this session. This report does not mark the feature complete.

Preferred remains configured at **33,554,432 active bytes**. TenSecond is an unqualified alternative: eight independently page-rounded 441,000-frame stereo phrases require 862 pages per phrase × 8 × 4,096 = **28,246,016 bytes**. There is no third profile, allocation-failure fallback or change to a pool owning live phrases. Neither supported profile has passed a physical reserve/deadline check.

## Candidate constants and accounting

These are implementation constants and compiler/accounting evidence, not measured device fit or approved operating reserves.

| Component | Preferred bytes | TenSecond bytes | Basis |
|---|---:|---:|---|
| Active stereo signed 32-bit frames | 33,554,432 | 28,246,016 | `config.h`; eight bytes per frame |
| Page | 4,096 | 4,096 | 512 stereo frames |
| History candidate | 4,194,304 | 4,194,304 | Separate arena; 1,024 page cap |
| Snapshot before-images candidate | 4,194,304 | 4,194,304 | Separate arena; 1,024 page cap |
| Mandatory provisional rollback | 3,704,448 | 3,704,448 | `requiredBytes()`; Task 7 host/ARM compiler ABI probe |
| Fixed snapshot staging | 4,096 | 4,096 | At most 512 copied frames per staged block |
| Five-arena subtotal | **45,651,584** | **40,343,168** | Before allocator overhead |
| PagePool startup metadata | 294,912 | 248,256 | Two existing metadata allocations; placement needs measurement |
| Fixed target fixture/session metadata | 39,016 | 39,016 | Task 7 ARM ABI probe; enabled symbol/map reconfirmed |
| Candidate accounted total | **45,985,512** | **40,630,440** | Excludes overhead and all other firmware demand |
| Permitted minimum free memory | **UNQUALIFIED** | **UNQUALIFIED** | No measured internal/external lows or threshold |

All five arenas are disjoint. History and snapshots cannot replace mandatory gesture storage. Active allocation uses the existing foreground pinned low-speed allocator path; runtime range/header checks are present but have not run on a device. PagePool metadata may use internal RAM; fixed fixture metadata resides in `.sdram_bss`. Allocator headers, rounding, fragmentation, stacks, streaming, outputs, voices, FX, menus and archive operations still require measured headroom. Neither nominal unallocated SDRAM nor static section sizes establish that headroom. No numeric minimum-free threshold was invented or added to configuration.

## Host work bounds

The new tests exercise real PagePool, ResidentAudio, Engine, rollback, history and snapshot code. No production fix was justified: all four cases passed after correcting one fixture arithmetic error. The initial wrong expected value was 639 for phrase index 639; the fixture stores index + 1, so the correct untouched value is 640. That failure was not a production regression.

| Missing combined stress case | Verified behavior and enforced bound |
|---|---|
| Eight simultaneous page crossings, both observers | Eight 1,024-frame phrases fill all 16 active pages. Rendering 128 frames from position 511 copies two pages per track per observer: 32 total copies, 16,384 frames / 131,072 bytes. Snapshots retain both boundary originals; the next span copies no additional pages. Per track this window permits at most 163,840 rollback tree steps including observer reads, and 128 protected frames. |
| Eight last-sample double taps | Each track captures 15,434 distinct provisional frames and cancels at elapsed sample 15,434. Dispatch permits at most 128 retirements and 32,768 tree steps (two 64-credit Engine services), never an entire scratch drain. Effective audio restores immediately. All 123,472 entries restore within the conservative 123,480 global fair-credit limit; each credit retires at most one entry with at most 256 tree steps. |
| Timer wrap during a gesture | Real `SampleClockExtender` observations cross the 32-bit firmware timer wrap. Elapsed sample 15,434 cancels; another slot at 15,435 confirms. Original/confirmed audible samples are checked. Overflow of the 64-bit extended clock is not claimed. |
| History-disabled saturated one-frame loops | Eight tracks each wrap 15,434 times at INT32 saturation with empty history/snapshot arenas. Each sample visit permits at most two tree steps and zero retirement work; only eight original frames are protected. Cancellation does no tree walk, restores effective audio immediately, and all originals retire with eight fair credits. |

These are operation-count bounds, not PMU timing or proof of deadline fit. The crossing bound allows 128 ordinary writes at the prior 1,024-step ceiling plus four 512-frame observer copies at a conservative 16 lookup steps per frame; each slot has at most 128 protected entries in this test. BeforeImages also performs fixed bounded metadata scans, not timed by these counters. Arbitrary callback crossings depend on frame count and phrase lengths. Whole-page copies remain synchronous; splitting them would require measured deadline evidence.

The provisional interval is 15,435 samples (350 ms at 44.1 kHz). Existing reuse coverage bounds a slot by at most 15,435 further writes, or 123,480 fair global credits for eight full slots. The new late-cancel test confirms the combined case. Actual service cadence and worst-case scratch reuse wall time remain NOT_RUN.

Each new test was separately run red with a temporary deliberate regression: skip snapshot preservation after the crossing (16 expected copies became 8); drain 123,480 entries in Engine service (128-retirement dispatch bound failed); discard the timer high word (extended sample assertion failed); overwrite the first original on each wrap (cancel returned INT32_MAX instead of 1). Each probe exited 1 with an assertion failure. All mutations were restored before green runs and firmware builds. They demonstrate test sensitivity, not bugs found in the unchanged implementation.

## Verification

All commands below completed with exit zero after restoring the deliberate mutations:

```text
cmake --build build/tests --target LoopStationTests -j4
LoopStationTests -sg LoopMemory           12 tests / 494,589 checks passed
LoopStationTests -sg LoopPages            10 tests / 245 checks passed
LoopStationTests -sg LoopGestureRollback  16 tests / 536,566 checks passed
LoopStationTests -sg LoopEngine           38 tests / 7,395 checks passed

cmake --build build/tests -j4
ctest --test-dir build/tests -C Debug --output-on-failure -j4
  16/16 passed (21.21 s), including all four allocation guards

cmake --build build/tests-loop-ubsan --target LoopStationTests \
  LoopGestureAllocationGuard LoopHistorySnapshotAllocationGuard -j4
build/tests-loop-ubsan/loop_station/Debug/LoopStationTests
  130/130 passed; 3,431,406 checks; no sanitizer diagnostics
build/tests-loop-ubsan/loop_station/Debug/LoopGestureAllocationGuard
build/tests-loop-ubsan/loop_station/Debug/LoopHistorySnapshotAllocationGuard
  both exit 0; no sanitizer diagnostics

DBT_NO_SYNC=1 ./dbt configure -DLOOP_STATION_TEST_MODE=ON
DBT_NO_SYNC=1 ./dbt build debug
DBT_NO_SYNC=1 ./dbt configure -DLOOP_STATION_TEST_MODE=OFF
DBT_NO_SYNC=1 ./dbt build debug
```

Both Debug links passed. ON: text 1,822,584 / data 70,336 / bss 450,684 bytes; OFF: text 1,803,008 / data 70,332 / bss 411,668 bytes. Internal `.bss` remains 199,376 bytes in each. ON `.sdram_bss` is 131,932 bytes at `0x0c0bed60`; OFF is 92,916 bytes at `0x0c0be9e0`. The ON fixture symbol is 39,016 bytes at `0x0c0d45c0`; OFF has no `loop_station::` symbols. These reconfirm static placement only. Both links emitted the existing LOAD-segment RWX warning; configuration warned that Doxygen is unavailable for documentation generation.

The local option is restored OFF. Task 7 cached dependency overrides were retained; no dependency/toolchain change was made. Host durations are host wall time under concurrent verification, not device timing. CSV validation confirms 40 rows with 43 fields; every physical `result` and runtime measurement field is `NOT_RUN`, while `host_evidence` contains software test references; `git diff --check` passed. Raw logs, maps, symbol/section outputs and mutation probes live in `.superpowers/sdd/2026-11-05-loop-station/task-8-*` (local ignored evidence); this committed report retains the results.

## Physical matrix — every row NOT_RUN

[acceptance.csv](../../../tests/hardware/loop_station/acceptance.csv) contains 40 rows: the ten cases below × OLED/seven-segment × Preferred/TenSecond. TenSecond rows are a qualification template, not authorization to select it without measured Preferred reserve failure. Every runtime field and result is explicitly NOT_RUN. Byte fields repeat unqualified accounting candidates; display labels identify required variants, not available boards.

| Workload | Required physical evidence | OLED | Seven-segment |
|---|---|---|---|
| Preferred attempt / explicit profile boot | Actual five ranges/regions/non-stealable headers, allocation result, metadata placement, overhead and free lows | NOT_RUN | NOT_RUN |
| Eight long phrases, archive disabled | Eight 524,288-frame Preferred phrases; if reduction justified, eight complete 441,000-frame TenSecond phrases; final-frame readback and output capture | NOT_RUN | NOT_RUN |
| Mixed long/short phrases, all gates open | Simultaneous crossings and many one-frame wraps, no storage servicing, continuity and full callback maxima | NOT_RUN | NOT_RUN |
| Rapid stop/start | Late taps, gate transitions, transport stops, phase and scratch reuse under sustained load | NOT_RUN | NOT_RUN |
| SD removed | Admitted capture/playback continues without storage progress, physical output and deadline evidence | NOT_RUN | NOT_RUN |
| SD stalled | Real yielding write/sync latencies, cancellation/queue pressure and output continuity | NOT_RUN | NOT_RUN |
| Normal song, outputs, FX and menus | Named supported streaming/song/voice/FX/menu load and measured operating reserves | NOT_RUN | NOT_RUN |
| Allocator/history/snapshot pressure | Exhaustion/evictions/cancellations, low watermarks and repeated enable/disable reclamation | NOT_RUN | NOT_RUN |
| Full scratch rollback/reuse | Actual command/render maxima and finite retirement/reuse latency at declared service cadence | NOT_RUN | NOT_RUN |
| Physical DMA/input/gain | Physical impulse delay, DMA alignment, absolute left/right/balanced/stereo gain, gate-close tail and recorded samples | NOT_RUN | NOT_RUN |

For each row record board model/revision/serial and display firmware; flashed commit/binary/build flags; card model/capacity/filesystem/firmware or removal/stall setup; song/voice/output/FX/menu configuration; phrase lengths; input/source/monitor/gain settings; sample rate, buffer/DMA cadence and PMU frequency; run duration; actual allocator ranges/overhead and internal/external low watermarks. Record the chosen numerical minimum free reserve, full callback/fixture maxima, actual deadline cycles and misses, copied pages/frames, history pressure, snapshot cancellations, accepted frame totals per track, storage calls and maximum write/sync latency, scratch retirement/reuse samples, input delay/gain convention, discontinuities and captured-audio evidence. Missing data must never mean zero or pass.

The Task 8 feasibility fixture supplies synthetic `{4096,-4096}` input at 44.1 kHz, equal-length phrases, immediate start/stop, all-track capture toggles every 44,100 frames, archive disabled, and discarded loop output. Separate product host tests now exercise unequal lengths, but they do not add mixed-length control to this debug fixture. Its PMU counter covers fixture work only. It lacks a firmware SD writer, full callback/deadline/heap watermark instrumentation, mixed-length control and physical input/output routing. The injected one-block archive adapter is not a real SD-stall test. Physical qualification therefore needs appropriate debug workload/instrumentation plus external audio capture; host plumbing cannot substitute. No software adapter or host fixture can fill that physical measurement gap.

## Gate decision

| Required condition | Available evidence | Hardware result |
|---|---|---|
| Permitted active pool and measured reserves fit | Accounting/map only; neither profile measured dynamically | NOT_RUN |
| Admitted capture continues without storage | Host tests; no removed/stalled-card output capture | NOT_RUN |
| Finite rollback/reuse under real cadence | Algorithmic bound; no hardware latency | NOT_RUN |
| No full audio deadline misses in declared base workload | No actual callback/deadline/output measurement | NOT_RUN |
| Physical input delay and gain established | Synthetic input only | NOT_RUN |

**The hardware gate has not passed.** Preferred remains configured and the runtime feature stays default-Off. The physical matrix remains NOT_RUN; all hardware/display work was explicitly deferred. Any future qualification requires actual measurements for the named display, profile and load, and one measured variant would not qualify the other.

The smallest proposed requirement correction is scheduling: defer physical Task 8 until devices/equipment are available, preserving the gate and capacity requirement. No capacity reduction is proposed from host accounting. Only measured reserve failure of Preferred can justify qualifying TenSecond. If both permitted profiles fail measured fit/deadlines, return with evidence for a requirement discussion; do not introduce a smaller pool or limit repeated admitted overdubs.
