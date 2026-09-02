# Loop Station architecture

Loop Station is split into firmware-independent and integration layers:

- `model/` owns sanitized song settings, clip bindings, revision descriptors,
  immutable phrase metadata, and revision cursor semantics.
- `engine/` owns gesture/state transitions, 64-bit song-tick scheduling,
  quantization, stop/resume behavior, capture draining, and error recovery. All
  side effects cross `EngineDelegate`.
- `audio/` owns the 16-channel recorder budget, physical-input fan-out and
  monitoring priority, dry cumulative overdub mixing, transpose ratios, and
  tick-based gain ramps.
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
- Capture reservations end when writing ends, independently of asynchronous SD
  finalization.
- An error keeps the previous stable phrase and requires an acknowledgement
  press before performance gestures resume.

The host tests compile these layers without display, hardware, recorder, or
filesystem dependencies. Firmware integration stores sanitized settings on
`Song` and the stable slot binding on `AudioClip`.
