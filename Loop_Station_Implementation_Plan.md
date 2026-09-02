# Loop Station Implementation Plan

  ## Summary

  Implement the complete eight-track Loop Station behind the default-off EnableLoopStation community feature flag. Build it as a new layered implementation on the current
  main-based branch, selectively reusing proven ideas from the old experimental branch without cherry-picking its generated files or incompatible audio path.

  Key decisions:

  - Entry is available only from Song/Session view using Cross-Screen Edit.
  - If disabled, show “Enable in Community Features.”
  - Reject entry while Arrangement playback or recording remains active.
  - Back each slot with a lazily created, visible Audio Clip and independent Audio Output named LOOP 1 through LOOP 8.
  - Global settings are Record Action, Record Quantize, and Start From Stopped.
  - Guarantee eight simultaneous stereo Loop Station captures under normal supported hardware/SD conditions.
  - Record every revision dry, untransposed, unnormalized, pre-FX, pre-level, and pre-pan.
  - Loop Sync controls both grid-aware resume and tempo-following time-stretch.
  - Completed loops continue playing after leaving the view; active captures/commits delay exit.

  ## Architecture and Internal Interfaces

  - Create the layered implementation under src/deluge/loop_station/:
      - model: enums, sanitized global/per-track settings, revision descriptors, and song-owned history.
      - engine: deterministic state machine, quantization, scheduling, phase math, undo/redo, and error recovery with no hardware, display, file, or recorder
        dependencies.

      - audio: physical-input fan-out, recorder budgeting, dry cumulative overdub tap, gain ramps, and combined pre-FX transpose.
      - playback: bridge from swung scheduler events to the engine.
      - settings: generic menu adapters for global and selected-track settings.
      - view: RootUI, gesture coalescing, recorder ownership/delegate, pad rendering, OLED, and hardware LEDs.

  - Define the core model types:
      - SlotId for exactly eight slots.
      - TrackState: EMPTY, RECORDING, OVERDUBBING, PLAYING, STOPPED, COMMITTING, and ERROR.
      - Separate CapturePhase: none, initial take, active overdub, finishing overdub, or finalizing. This permits the UI to show PLAYING while an overdub recorder
        finishes in the background.

      - Global settings: RecordActionMode, RecordQuantizeMode, and StartFromStoppedMode.
      - Track settings matching the specification: physical source, monitoring, duration, playback mode, Loop Sync, start/stop mode, fade bars, semitones, cents, and
        progress colour.

      - Runtime data: pending event, phrase length, immutable playback origin, saved stop phase, overdub start/end ticks, history size/cursor, commit dependency, and
        recoverable error state.

  - Define an EngineDelegate contract through which the view/controller performs all side effects:
      - Preflight and reserve recorder resources.
      - Begin initial or overdub capture.
      - End capture and report asynchronous finalization.
      - Abort provisional capture.
      - Publish or reject a completed revision.
      - Navigate or clear revision history.
      - Apply stable-state changes to the backing Audio Clip.

  - Store timing as absolute 64-bit song ticks and phrase lengths as bounded positive tick counts. Use scheduler-derived performed ticks for gestures and exact swung
    events for queued actions. The engine exposes its next required event so Session playback can reduce swungTicksTilNextEvent.

  - Use the firmware-native fixed-point unity value, kMaxSampleValue (Q24), for transpose ratios instead of the spec’s incorrect literal 65536.

  ## Implementation Changes

  1. Specification, feature flag, and entry

  - Update LOOP_STATION_SPEC.md before implementation to reflect the locked behavior:
      - Song/Session view is the only entry point; remove Arranger-view entry claims.
      - Arrangement playback/recording makes Loop Station unavailable.
      - Add configurable Record Quantize to the global settings table and remove undefined Global Sync references.
      - Define playback fades, Loop Sync tempo behavior, error acknowledgement, visible backing clips, and the eight-stereo capacity requirement.
      - Correct transpose fixed-point terminology.

  - Append EnableLoopStation to runtime feature settings, default Off, with XML key enableLoopStation and generated OLED/seven-segment localization.
  - In SessionView, consume an unchorded Cross-Screen Edit release only when no incompatible UI mode is active:
      - Disabled: show the enable-feature popup.
      - Arrangement engine active: show an unavailable popup.
      - Otherwise: open LoopStationView.

  - Leave Arranger’s existing Cross-Screen auto-scroll behavior unchanged.
  - Cross-Screen Edit or the Song button exits Loop Station. BACK remains reserved for history navigation.

  2. Engine behavior and gestures

  - Coalesce the four physical LEDs in each 2×2 centre into one logical gesture. Execute performance actions on the first down event except STOPPED, whose action waits
    for release to distinguish a tap from the two-second clear hold.

  - Recognize a second centre press within 350 ms as the secondary gesture without delaying the first press. Track the original gesture transaction so a double press can
    abort an overdub that the first press just started or moved into background finishing.

  - Implement these transitions:
      - EMPTY → RECORDING: Off/Smart start immediately; Measure starts at the next bar, with a press exactly on a boundary starting there.
      - RECORDING → COMMITTING/PLAYING: Off stops at the performed tick; Smart crops or extends to the nearest bar only within an eighth note; Measure targets the
        following bar. Fixed duration uses captureStart + bars × barLength.

      - PLAYING → OVERDUBBING: immediate for looping tracks; one-shot instead retriggers phase zero.
      - OVERDUBBING → PLAYING + finishing capture: a primary press changes the presentation immediately while input continues recording silently until the exact phrase
        end.

      - Double press while playing/overdubbing aborts any provisional overdub and applies the configured stop behavior.
      - STOPPED → PLAYING: From Start retriggers phase zero immediately. Resume Quantized queues to the next bar and restores the song-aligned phase when Loop Sync is On;
        an unsynced track resumes its saved stop phase immediately.

      - Hold STOPPED for two seconds: clear all revisions but retain the clip binding, name, Output, FX, and settings.
      - ERROR: keep the previous committed phrase audible; the next centre press only acknowledges the error and restores the saved stable state.

  - Apply start/stop settings only to playback:
      - Start Fade ramps the backing loop from zero to unity over fadeTimeBars.
      - Stop Loop End queues the stop at the next phrase boundary.
      - Stop Immediate uses the existing anti-click ramp before deactivating playback.
      - Stop Fade ramps to zero over fadeTimeBars, then enters STOPPED.
      - Recording-to-playback and overdub boundaries do not receive these user fades.
      - Gain ramps are tick-based so tempo changes remain smooth; FX tails may decay after the dry loop reaches zero.

  - For RECORD_OVERDUB_PLAY, bind the provisional first take and start the automatic overdub exactly at the first phrase boundary even if the first WAV is still
    finalizing. The overdub revision depends on the parent commit; failure of the parent aborts the dependent overdub and restores empty/error safely.

  - Intercept a transport-stop request during capture:
      - Finish initial recordings according to their quantization rule.
      - Let overdubs reach their mandatory phrase end.
      - Keep the internal clock running until captures and commits finish, then stop transport and exit the view.

  3. Audio capture, playback, and resource management

  - Add a RecorderBudget with 16 channel units: mono costs one and stereo costs two. It must admit eight simultaneous stereo Loop Station captures. Release the active-
    capture reservation when capture ends, even while SD finalization continues.

  - Preflight recorder objects, first clusters, revision metadata capacity, source validity, and pitch-shifter resources before changing the track state. Runtime
    allocation, SD, or file-size failures affect only the requesting slot.

  - Add a per-audio-buffer physical input router:
      - Read LEFT, RIGHT, STEREO, or BALANCED once.
      - Fan the dry source to every active capture.
      - Route monitoring to capturing tracks first and the selected idle track second.
      - Suppress an idle monitor if another track is capturing the same source.
      - Deduplicate identical monitor returns while retaining separate processing when transpose/FX signatures differ.

  - Split the bound Audio Output render into explicit dry and audible paths:
      1. Render the current revision at unity into a dry backing buffer.
      2. Obtain the selected physical source as a dry input buffer.
      3. For overdub capture, saturating-add backing and input into a private recorder buffer before all transpose, fades, FX, level, pan, compression, and normalization.
      4. For audible output, apply playback gain/fade to backing, add only the permitted monitor return, pass the combined signal through transpose, then through the
         existing Audio Output FX chain.

  - During FINISHING_OVERDUB, continue writing physical input into the cumulative revision but remove it from the audible monitor path, as specified.
  - Preserve silence overdubs sample-for-sample at unity when source and system sample rates match. Latch backingMissing if any capture buffer cannot render the prior
    revision; reject the entire provisional WAV rather than publishing input-only audio.

  - Choose the cumulative revision’s channel count as stereo if either its backing revision or current source is stereo; otherwise retain mono. Duplicate mono backing/
    input into both channels only when producing a stereo revision.

  - Keep the old revision playing throughout overdub and SD finalization. Publish a successful new revision at an audio-buffer boundary, position it at the current
    logical phase, and use the anti-click crossfade for the swap.

  - Model phase as:
      - logicalPhaseTicks = floorMod(currentTick - playbackStartTick, phraseLengthTicks).
      - Each revision stores the logical phase represented by physical sample zero.
      - Convert the rotated tick phase into sample frames using that revision’s actual frame count and phrase length.
      - Never alter the original playback origin or phrase length after the first accepted take.

  - Generalize the existing granular live pitch-shifter so each Loop Station track can feed an arbitrary combined pre-FX buffer rather than only a global physical-input
    buffer:
      - One shifter instance per transposed track handles both loop playback and audible monitoring.
      - Create it outside the audio thread whenever a phrase or audible monitor needs non-zero transpose.
      - Recreate/reset it after semitone/cents changes and crossfade into the new state.
      - Use fixed 8192-frame circular buffers and up to 64 interpolation samples.
      - Zero transpose bypasses the shifter completely.
      - Never feed transposed output to a recorder.

  - Loop Sync behavior after tempo changes:
      - On: time-stretch the loop to preserve its fixed musical tick length and phase.
      - Off: play at recorded speed and free-run; tick quantization affects launch only, not subsequent sample boundaries.

  4. Persistence, backing clips, UI, and menus

  - Extend each Audio Clip with a persisted loopStationSlot="1"…"8" binding; zero/unset means ordinary clip. Generic cloning clears the binding.
  - Store song-level Loop Station data in a version-tolerant loopStation block:
      - Global settings.
      - Eight per-track settings records.
      - Per-track history cursor and ordered revision list.
      - Each revision stores file path, sample bounds, phrase length in ticks, and sample-zero phase in ticks.

  - Revision history owns an independent Sample reason for every WAV so undo/redo files survive and temporary captures participate in normal Save Song relocation.
  - Reserve append metadata before starting capture. Truncate a redo branch only after the replacement revision has successfully finalized.
  - On load:
      - Sanitize all enums and ranges.
      - Clamp cursor to [-1, revisionCount - 1].
      - Resolve the first valid Audio Clip binding per slot and clear duplicates.
      - Load all history Samples after the main song file closes.
      - Reconstruct the current revision and derive a runtime playback origin from the clip’s current phase rather than serializing an obsolete absolute tick.
      - Missing/corrupt revisions do not replace the current playable revision.

  - Deleting a visible bound clip unbinds its slot and releases its complete history. Clearing from Loop Station retains the empty backing clip and Output.
  - Lazily create a backing clip when an empty slot is selected, configured, or first recorded. Assign each its own Audio Output so track FX, mute state, name, and gold-
    encoder controls remain independent.

  - Implement the 4×2 grid, 4×4 cell geometry, 2×2 centre button, clockwise 12-LED border, beat pulses, progress interpolation, selection lift, pending-action blink,
    state colours, REC LED aggregation, and BACK history LED.

  - Selection sets the normal active modulation target to that slot’s Audio Clip/Output; deselection restores the Song target.
  - Map BACK to undo and SHIFT+BACK to redo for the selected non-busy track.
  - SELECT opens the global menu. SHIFT+centre opens the track menu containing Source, Monitoring, Duration, Playback, Loop Sync, Start/Stop Mode, Fade Time, Transpose,
    Progress Colour, and a single direct entry to the ordinary Audio Output FX suite.

  - Snapshot capture-affecting settings at capture start. Source/duration/playback changes apply to the next take; monitoring and transpose changes can update the live
    audible path safely.

  - Show Loop Station and the selected clip name/track number in the OLED title, with localized seven-segment abbreviations.

  ## Test and Acceptance Plan

  - Add host unit coverage for:
      - Defaults, range sanitization, and parameter adapters.
      - Every state transition, both record-action modes, double presses, clear hold, error acknowledgement, and per-track independence.
      - Off/Smart/Measure boundary cases, fixed duration, maximum length, queued stop/fade events, and transport-stop draining.
      - One-shot behavior and grid-aware stopped resume.
      - Mid-phrase phase rotation, tick-to-frame conversion, tempo changes with Loop Sync on/off, and revision swaps.
      - Unlimited history navigation, empty undo state, failed navigation, redo truncation only after commit, and failure preservation.
      - Saturating addition, bit-perfect silent overdub, backing-missing rejection, mono/stereo promotion, source fan-out, monitoring priority/deduplication, and a
        successful eight-stereo budget reservation.

      - Transpose ratio limits, unity bypass, combined playback/monitor processing, lifecycle resets, and proof that recorder buffers remain dry.

  - Add firmware/integration tests for song XML round trips, duplicate bindings, legacy files without Loop Station data, missing WAVs, temporary-file relocation, deleting
    a bound clip, and song switching.

  - Perform hardware acceptance with all eight stereo tracks simultaneously recording and overdubbing:
      - Test every physical source, mixed mono/stereo histories, active FX, extreme ±24.5-semitone transpose, external and internal clocks, tempo changes, and slow/near-
        full SD cards.

      - Assert no audio callback overruns, zero-filled buffers, voice loss, phrase-boundary discontinuities, or audible dropout during commit/swap.
      - Inject allocation, SD-write, maximum-file-size, and backing-render failures and verify only the affected slot errors while its previous revision remains audible.
      - Treat the eight-stereo no-glitch stress test as a release gate, not an aspirational benchmark.

      - ./dbt build debug

  - Finish by updating the architecture deep document, Community Features documentation, and a user-facing Loop Station guide covering gestures, settings, visible backing
    clips, history, storage use, and the Arrangement limitation.

  ## Assumptions

  - Song view means the existing SessionView; Arranger code and its Cross-Screen behavior remain untouched.
  - Opening is rejected whenever the Arrangement playback engine is active, even if Song view is currently displayed.
  - The feature ships complete rather than leaving fades, transpose, or persistence as placeholder settings.
  - One-shot playback always starts from phase zero; Start From Stopped resume semantics apply to looping playback.
  - Eight-stereo acceptance assumes supported hardware and an SD card meeting the firmware’s normal recording requirements; unexpected global RAM or SD failure still
    follows the per-slot error contract.

  - The prior experimental feat/loopstation branch is reference material only. Implementation starts from the current branch and ports only reviewed logic that satisfies
    the revised specification.
