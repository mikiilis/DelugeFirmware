# Loop Station standard controls

Status: approved by the user on November 15, 2026. Implementation and hardware acceptance remain pending.

## Intent

Make applicable standard Deluge controls available without leaving Loop Station. The user reports blocked transport, recording, metronome and effects controls, and selected normal Deluge PLAY/RECORD behavior rather than retaining the selected-loop RECORD shortcut.

## Findings

- `LoopStationView::buttonAction` consumes unmatched events instead of calling the shared `view.buttonAction` handler. This blocks tap tempo, metronome, Song Save/Load and other shared shortcuts.
- It remaps PLAY to loop Start All and RECORD to selected-slot Press, bypassing the native button dispatcher's transport and recording handling.
- Inherited modulation handlers already forward to `view`, but Loop Station does not set the active modulation target on entry or selection.
- Native transport stop already calls `SongModel::requestTransportStop`, closing loop captures and stopping phrases independently of loop stop quantization.

These are code-inspection findings, not hardware reproduction or acceptance.

## Approaches

1. **Focused shared-handler integration — proposed.** Retain the existing RootUI and loop-specific controls; delegate applicable ordinary buttons to `view.buttonAction`, allowing unhandled native events to reach the button dispatcher. Explicitly bind the modulation target. This reuses established behavior without importing unrelated editing modes.
2. **Delegate to SessionView or inherit its timeline behavior — rejected.** Its grid, clip-creation, scrolling, zoom and arrangement assumptions do not match Loop Station's tiles.
3. **Reimplement standard controls locally — rejected.** Duplicates transport/record modifier handling and risks diverging again.

The user selected normal physical transport/record controls; focused integration is the builder's proposed implementation approach.

## Behavior

- PLAY and RECORD use their normal shared/native handlers, including button releases and existing supported modifiers. RECORD no longer acts as a selected loop's centre press. Bound resident clips must retain their existing protection against ordinary linear audio recording.
- PLAY toggles normal song transport. It is not an alias for loop Start All. Starting transport alone does not add a new automatic loop-group launch behavior; centres and the sidebar remain the loop launch controls. Stopping transport closes loop capture gates and stops loops immediately through the existing transport-stop path.
- Tap tempo, SHIFT + TAP metronome, tempo-button actions, Song Save/Load and SHIFT + SELECT device settings use existing shared behavior where applicable.
- Existing modulation buttons, gold knobs and their presses target the selected loop's ordinary clip/output effects. AFFECT ENTIRE toggles song-wide effects, returning to the selected loop when switched off. Refresh the target and indicators on entry, focus regain and selection changes. With no selected loop, use a safe song target rather than an unrelated previous clip. This fallback is a builder proposal.
- Preserve the eight tiles, centre-pad recording/overdub gestures, sidebar Start All/Stop All, unshifted SELECT loop settings, SHIFT-centre track settings, loop undo and existing safe exit/Clip View navigation.
- Loop-specific actions must respect shared modal states introduced by Save/Load and MIDI learn; a held shared shortcut must not accidentally trigger a loop action. Preserve SD-card deferral and paired press/release behavior.
- Effects continue processing through existing outputs; recording remains dry. No changes to phrase length, audio ownership, archive behavior or capture timing.

## Scope and constraints

This is a focused input/context integration repair, not a new sequencer or a redesign of loop gestures. 'Like other views' means standard controls that apply to this view, not importing note editing, synth/kit conversion, timeline zoom or arrangement recording shortcuts indiscriminately. Existing controls in other views must remain unchanged. Do not overwrite unrelated working-tree changes.

## Verification

Add regression coverage for shared/native event routing, PLAY/RECORD press and release, metronome and modifier routing, retained loop shortcuts, and effects targeting across selection/AFFECT ENTIRE/focus changes. Check native transport stop and resident recording guards. Run relevant host tests and compile firmware. Update both Loop Station guides to distinguish native transport/record controls from loop pads/sidebar. Hardware acceptance must separately exercise playback, stopping during capture, recording/count-in, metronome, track/song effects, Save/Load and return from menus; do not claim physical success from host tests alone.
