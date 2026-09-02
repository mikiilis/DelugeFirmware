# Loop Station architecture

Loop Station is split into firmware-independent and integration layers:

- `model/` owns sanitized song settings, clip bindings, revision descriptors,
  immutable phrase metadata, and revision cursor semantics.
- `engine/` owns gesture/state transitions, 64-bit song-tick scheduling,
  quantization, stop/resume behavior, capture draining, and error recovery. All
  side effects cross `EngineDelegate`.
- `audio/` owns recorder-cost calculation, physical-input fan-out and monitoring
  priority, dry cumulative overdub mixing, transpose ratios, and tick-based gain
  ramps. `AudioEngine` enforces the shared 16-channel capacity across Loop
  Station and every other recorder client.
- `playback/` is the narrow bridge used by the swung Session scheduler.
- `settings/` exposes bounded adapters shared by menu code and tests.

## Invariants

- There are exactly eight independent slots.
- Capture is always dry and occurs before transpose, fades, FX, level, and pan.
- A stereo recorder costs two channel units and a mono recorder costs one;
  eight stereo reservations fit simultaneously.
- The first accepted take fixes the phrase length and playback origin.
- A cumulative revision records the logical tick phase represented by sample
  frame zero. Revision swaps rotate samples, never the musical origin.
- Redo metadata is truncated only when its replacement commit succeeds.
- Recorder capacity remains charged until asynchronous SD finalization and close
  complete; a slot cannot be recorded again while its commit is pending.
- A revision is provisional until its WAV finalizes successfully. Failed commits
  restore the previous durable revision without truncating redo history.
- An error keeps the previous stable phrase and requires an acknowledgement
  press before performance gestures resume.

The host tests compile these layers without display, hardware, recorder, or
filesystem dependencies. Firmware integration stores sanitized settings on
`Song` and the stable slot binding on `AudioClip`.
