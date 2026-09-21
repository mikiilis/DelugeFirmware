# Enable complete audio controls, ADSR, and arpeggiation for Loop Station tracks

  ## Summary

  Give each of the eight loops access to every applicable audio grid control, a full amplitude ADSR envelope, and an arpeggiator with an editable pitch pattern.

  The current limitation comes from the playback engine: loops use Audio Clips, which expose Attack but lack instrument-style ADSR and arpeggiator playback. This requires
  changes to playback, parameter handling, menus, and persistence.

  The agreed scope preserves Audio Clips and overdubbing. Synth oscillators, FM, additional modulation envelopes, and synth modulation routing remain outside this change.

  ## Settings and grid access

  - Extend Track Settings with Envelope and Arpeggiator menus. Keep the existing sample, master, filters, compressor, sidechain, FX, and priority controls.
  - Derive loop shortcuts from the existing audio map, adding the printed ENV1 Attack/Decay/Sustain/Release positions and Arp Mode/Rate/Sync/Gate/Octaves positions.
  - Include applicable secondary shortcuts, such as compressor ratio and stutter direction. Audit both shortcut layers against the menu tree so every supported mapped
    control is reachable.

  - Use the same shortcut resolution for pad presses, blinking, and horizontal-menu navigation, including supported shortcut-layout versions.
  - Bind edits to the loop whose settings were opened. Opening another loop’s settings must not change monitoring selection or edit the previously selected loop.
  - Add audio-safe envelope and arp menu adapters. Existing synth menus cannot simply be inserted: several assume a valid currentSound or patched synth parameters.
  - Support OLED horizontal menus and the existing vertical/7-segment navigation.

  ## Playback behavior

  Envelope

  - Provide one amplitude ADSR per loop, using the existing envelope curves and parameter scales.
  - With arp off, Attack and Decay run when playback starts; Sustain continues across repetitions; Release begins at the scheduled stop boundary.
  - With arp on, every generated note receives its own ADSR. Gate controls note-off and therefore the start of Release.
  - Keep launch/stop fades as an additional gain stage. Ordinary stopping permits an envelope tail; a completed stop fade silences any remaining tail. Clear and emergency
    cut use existing click suppression.

  Arpeggiator

  - Retrigger the recording from its playback start, respecting markers, reverse playback, and revision phase offsets. Each note adds its pitch offset and arp octave
    shift to the existing transpose behavior.

  - Expose Mode, Rate, Sync, Gate, Octaves, note/octave order, step repeats, rhythm, sequence length, and the applicable monophonic randomizer controls.
  - Add Notes → Length / Step / Semitones using normal Select/Back navigation: 1–16 entries, each −24 to +24 semitones, default [0]. Repeated pitches are allowed. Custom/
    As Played follows entry order; Up/Down follows pitch order.

  - Play one note at a time. A new note replaces the previous note with a short transition to prevent clicks; chord tones are arpeggiated sequentially.
  - Reuse the existing arp timing and pattern engine through a bounded loop-note adapter. Override AudioOutput::doTickForwardForArp for synchronized events and service
    free-rate/gate timing during rendering.

  - Preserve the phrase’s transport phase, progress ring, recording duration, and one-shot endpoint independently of arp retriggers. Disabling arp returns to the current
    phrase position.

  Recording and resource handling

  - Introduce an audible sample player with its own VoiceSample playback state, independent of the canonical loop used for cumulative overdubs.
  - Capture the continuous dry loop plus physical input before arp, ADSR, transpose, fades, and FX. Mix live monitoring after loop shaping and before the existing shared
    FX chain.

  - Retain the existing fast path when the added shaping is inactive. Prepare resources outside audio rendering; use bounded voice counts and safe sample ownership across
    commits, undo/redo, replacement, and clear.

  - Continue playback and release tails independently of the open UI, including Session and Arrangement views.

  ## Data and compatibility


  - Retain the existing Attack value. Reuse existing unpatched arp parameter IDs and their automation/serialization paths, including the already-defined global arp rate.
  - Save additional configuration with the backing Audio Clip. Missing data defaults to arp Off, immediate Decay/Release, full Sustain, and [0], preserving existing
    songs’ audible behavior.

  - Keep added ADSR fields and note-pattern editing as saved settings; retain existing automation capabilities for reused audio/arp parameters.
  - Preserve settings through phrase clearing and revision changes. Ordinary clip cloning copies them; Copy To Track retains its current configuration-only behavior and
    does not acquire sound-setting copying.

  - Build on the existing staged Loop Station changes and update the specification and user documentation with the supported control map.

  ## Validation

  - Test shortcut/menu agreement across both layers, correct loop targeting, horizontal-menu navigation, empty loops, and unsupported synth pads.
  - Test ADSR stages, sustain across repetitions, arp gate/release interaction, rapid retriggers, and stop/fade behavior.
  - Test editable patterns, repeated pitches, octave ordering, synchronized/free rates, tempo changes, and live arp enable/disable.
  - Verify that arp and ADSR do not alter recorded WAV content, cumulative overdubs, phrase timing, or undo/redo results.
  - Test save/load, legacy defaults, clip cloning, clear, sample replacement, voice allocation failure, and resource cleanup.
  - Run the host unit and recorder regression suites and build Debug/Release firmware.
  - Validate on hardware with eight simultaneous loops, stereo overdubbing, active FX, rapid settings changes, and long release tails. Measure CPU/voice usage and check
    for underruns, clicks, freezes, and leaks before considering the feature complete.
