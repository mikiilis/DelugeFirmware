# Loop Station Recording Length Display

**Status:** Approved for inline implementation by the user's November 15, 2026 executing-plans request.

## Intent

Show the measured length of a completed phrase without mislabelling every short recording as `1 BAR`. This is display formatting only: preserve captured samples, playback, start/stop quantization, and all saved data.

## Approaches

1. **Always seconds:** accurate and tempo-independent, but loses useful musical lengths.
2. **Always nearest musical fraction:** readable, but misleading for free recordings and after tempo changes.
3. **Recognized musical lengths, otherwise seconds (recommended):** retain musical context where justified by the measured samples; use elapsed seconds otherwise.

The plan uses approach 3, following the prior recommendation. The detailed rules below are proposed engineering choices for the user's review, not previously confirmed preferences.

## Formatting contract

- Preserve the current three-line OLED layout, title, track number, input, BPM, and right-aligned length field. No separator or new screen controls.
- During initial recording or overdub, keep `REC`. An empty track stays `READY`. Live elapsed recording time is out of scope.
- Format completed phrases using their `FrameIndex` sample count, `kSampleRate = 44100`, and the current `ClockMap::tempoQ16()`. Do not infer length from Start/Stop settings or the rounded BPM label.
- Recognize lengths that are a positive integer multiple of either **1/32 bar** or **1/48 bar**, within **one audio sample** of their expected duration. These grids cover the existing straight and triplet interval families; reduce fractions using GCD. This is tolerance for sample rounding, not a new quantizer.
- Check the 1/32 grid first, then 1/48. Both grids give the same reduced fraction wherever they intersect.
- For a whole number of bars, use `1 BAR` / `4 BARS`. Otherwise use an improper reduced fraction: `1/16 BAR`, `3/16 BAR`, `3/4 BAR`, `3/2 BARS`. Use singular `BAR` below one bar; plural `BARS` above one bar. Do not add mixed-number notation.
- A musical label may occupy at most **8 ASCII characters**, including spaces. Longer musical labels fall back to seconds rather than shortening the input label or changing the layout.
- Otherwise show elapsed seconds rounded half-up to hundredths: `0.37 s`, `1.50 s`, `12.34 s`. Use integer arithmetic; no floating-point firmware formatting.
- A positive phrase that rounds below `0.01 s` shows `<0.01 s`, never `0.00 s`. A zero-frame input to the pure formatter shows `0.00 s`.
- If rounded hundredths reach 1000 seconds, show nearest whole seconds instead (`1000 s`, at most `97392 s` for uint32 frame counts). This preserves the 8-character limit for every `FrameIndex` value.
- A zero or above-1000-BPM Q16 tempo falls back to seconds, without dividing by it. Production ClockMap already rejects those inputs; the pure formatter remains safe independently.
- Musical labels describe duration at the **current tempo**, not the tempo at capture. A later tempo edit may therefore switch a phrase from fractions/bars to seconds. Seconds always derive from sample count.
- No phrase trimming, padding, resampling, changed quantization, persistence migration, new dependencies, or firmware selection-gesture changes.

## Examples at 120 BPM, four quarter notes per bar

| Captured samples | OLED length |
| ---: | --- |
| 1 | `<0.01 s` |
| 1838 | `1/48 BAR` |
| 5513 | `1/16 BAR` |
| 11025 | `1/8 BAR` |
| 16317 | `0.37 s` |
| 22050 | `1/4 BAR` |
| 44100 | `1/2 BAR` |
| 88200 | `1 BAR` |
| 132300 | `3/2 BARS` |
| 352800 | `4 BARS` |

## Implementation boundaries

- One pure, bounded C++ formatter shared by the production view and host tests.
- The view retains capture/empty precedence and delegates only completed phrase lengths.
- Both production-source probes include the same formatter header; no copied C++ algorithm in mocks or generators.
- The HTML demo mirrors the formatting with its illustrative seconds converted to 44.1-kHz frames. The demo is still silent and does not simulate firmware quantization.
- Keep `colors`, `ring`, pad animation, demo selection toggling, and the demo's illustrative minimum recording duration unchanged. Do not restart the stopped HTTP server.

## Acceptance

Host tests pin sample-rounding tolerance, straight/triplet fractions, off-grid and very short lengths, fractional/changed tempos, output width, and canvas clearing across successive renders. The ARM Release build must pass. Actual device legibility remains a separate hardware check, not something host tests establish.
