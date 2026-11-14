# Loop Station shared horizontal settings UI

Status: written design approved on November 14, 2026 through the explicit "Approve design" choice. The implementation plan and execution method still require review before firmware implementation.

## Intent and agreed direction

Make Loop Station settings use the familiar master-settings interaction instead of requiring another navigation pattern. The user requested four on-screen options selected by SYNTH, KIT, MIDI and CV; previous/next settings pages on SCALE and CROSS-SCREEN; and transpose editing in semitones normally and cents with SHIFT.

The user explicitly chose:

- Apply this to **both per-track and global Loop Station settings**.
- **Reuse shared horizontal-menu components**, not a separate implementation of the same UI.
- Preview the interaction in the **HTML already embedded in the documentation**, `website/public/demos/loop-station.html`.

The user accepted the resulting interactive preview. It remains a design prototype: no firmware changes have been made. The documentation demo clearly identifies the proposed settings behavior as unimplemented and temporary.

## Goals

- Share selection, paging, rendering and LED behavior with the existing horizontal menus.
- Edit an ordinary selected value directly by turning SELECT, without an edit-mode toggle.
- Expose all existing Loop Station settings, including Colour and Copy To Track.
- Make transpose practical for coarse musical changes and precise tuning.
- Preserve saved settings, track identity, audio and the Loop Station command/configuration path.

## Non-goals and anti-goals

- No changes to recording/overdub timing, transport, pitch processing, audio allocation, persistence schema or track defaults.
- No changes to the meanings, choices or ranges of existing settings, except the requested transpose encoder step sizes.
- No wholesale move of Loop Station into the sound editor; its separate UI context and backing-track checks remain.
- No new effects, modulation, MIDI learning or sound-editor pad shortcuts in this menu.
- Do not duplicate horizontal paging/rendering in `SettingsMenu` or turn the shared menu code into a Loop Station-specific implementation.
- Do not alter ordinary synth, kit, MIDI/CV, song or audio-clip menus as an incidental consequence.
- The prototype does not establish real OLED readability, physical input behavior, memory capacity or audible results.

## Current implementation and constraints

`src/deluge/gui/menu_item/loop_station/settings.{h,cpp}` defines a separate `RootUI`. It stores a selected row, an edit toggle and a Copy To Track mode. SELECT rotation either navigates rows or edits a value. Both global and track settings use this implementation. `visibleMenuFields` in `src/deluge/loop_station/menu_model.cpp` supplies the field order and conditionally exposes synced intervals. Changes go through `SongModel::dispatch(Command::configure(...))`; copying goes through `copySettingsTo`.

`src/deluge/gui/menu_item/horizontal_menu.{h,cpp}` already implements four-column menus, instrument-button selection, SCALE/CROSS-SCREEN paging, selected-column LEDs, page counters, conditional item visibility, direct child editing, notifications and accessibility highlighting. Its base `Submenu`, lifecycle, item initialization, pad-light updates and navigation currently reference the global `soundEditor`. It cannot safely be placed in the Loop Station host without addressing those dependencies.

`Decimal::getNumberEditSize()` already uses a 100-cent coarse step and one-cent fine step for a horizontal parent, with SHIFT or held SELECT enabling fine editing. Loop Station must continue to configure its own `TrackSettings::transposeCents`; the ordinary audio-clip transpose item edits a different sample-holder path and must not be reused as the data binding.

The shared menu normally renders horizontally only with an OLED and the existing `HorizontalMenus` setting enabled. Existing host tests include production-method UI probes and real OLED-canvas coverage; adding the shared dependency must not result in tests of a replacement mock implementation alone.

## Approaches

### Chosen: reuse shared components through a narrow host boundary

Retain the Loop Station settings host and use the existing `HorizontalMenu` machinery with Loop Station-specific menu items. Isolate the sound-editor-only services behind narrowly scoped hooks or a small host adapter, with current sound-editor behavior as the default. Share the real horizontal selection, paging and rendering rather than copying their algorithms.

Trade-off: requires careful shared-code integration and regression tests, but keeps controls and appearance aligned over time. The user selected shared reuse; the precise hook/adapter structure below is the author's proposed implementation direction.

### Not selected: locally reproduce the pattern

Extend `SettingsMenu` with its own four-column renderer, paging and instrument-button selection. This touches less shared code initially but duplicates behavior and risks future drift. The user did not select this approach.

### Not pursued: route settings through the complete sound editor

This would reuse the surrounding editor as well, but introduces unrelated clip/modulation/navigation assumptions into Loop Station's settings and widens the change unnecessarily. It is not needed to deliver the chosen interaction.

## Interaction design

### Entry, exit and live behavior

- Keep SELECT from the Loop Station root opening global settings.
- Keep SHIFT + a track centre opening that track's settings. Consume the centre action so this gesture cannot also record, overdub or clear audio.
- Start at the first relevant field each time the menu opens; retain the addressed track for the entire settings session.
- BACK closes settings, retaining already applied in-memory edits. SONG/Session View keeps the existing unshifted close behavior; SHIFT must continue reaching the shared button handler.
- CROSS-SCREEN is next page while settings is open, not exit Loop Station; SCALE is previous page.
- Preserve safe SD-card deferral and lazy track binding, including failure feedback. Opening/closing settings must not start or stop transport.
- In horizontal mode, pad presses do not confirm actions, change track or trigger underlying loop gestures. Do not inherit the old behavior where any pad acted like SELECT.
- Continue showing the underlying loop pads and their live updates. Restore the parent view's button LEDs on close, and clear all temporary menu LED states on failure/exit.

### Horizontal selection and editing

- SYNTH/KIT/MIDI/CV select columns 1/2/3/4 of the current page.
- Turning SELECT edits the selected ordinary value immediately. SELECT press is not an edit-mode toggle; ordinary Loop Station items must not navigate out or apply a hidden action when SELECT or their already-selected instrument button is pressed.
- Use the shared menu's title, page indicator, value/label rendering, accessibility highlighting, notifications, navigation debounce and selected-column LED behavior. Use compact localized column labels where necessary, with the full setting name available through the existing selected-item notification mechanism.
- Page navigation wraps. Retain the selected column when possible and clamp to the last relevant field on a shorter page. An unoccupied column is inert and must not select a field on another page.
- Conditional visibility changes keep the same selected **field**, recompute pages, and never leave an invalid selection or accidentally edit the adjacent field.
- No chained horizontal-menu navigation is introduced for Loop Station; holding SHIFT while paging still changes pages in this settings menu, never jumps to unrelated sound-editor menus.
- The existing horizontal encoder must not bypass the selected item's behavior or accidentally keep the former row-navigation/edit alias. Its precise routing is left to the builder, consistent with the shared horizontal menu and without additional shortcuts.

### Track pages

Each field occupies one column. Use this order, preserving the existing setting order:

```text
TRACK n SETTINGS                           1/2
  Input       Monitor       Duration       Playback
  SYNTH       KIT           MIDI           CV

TRACK n SETTINGS                           2/2
  Transpose   Colour        Copy To        [unused]
  SYNTH       KIT           MIDI           CV
```

Source offers the existing Left, Right, Stereo and Balanced choices. Monitoring offers Off/On. Duration uses Manual or the existing dynamically calculated maximum bars, not the prototype's illustrative eight-bar cap. Playback offers Loop/One Shot. Colour uses the existing eleven-colour palette, including White; duplicate colours remain allowed and changes update the appropriate ring live.

### Global pages

Use the shared dynamic layout in this order: Record Action, Start Mode, Start Interval, Stop Mode, Stop Interval. Start/Stop Interval is relevant only while its corresponding mode is Synced. With both synced:

```text
GLOBAL SETTINGS                            1/2
  Rec Action  Start Mode    Start Interval Stop Mode
  SYNTH       KIT           MIDI           CV

GLOBAL SETTINGS                            2/2
  Stop Interval [unused]     [unused]       [unused]
  SYNTH         KIT          MIDI           CV
```

Switching to Immediate hides that mode's interval and compacts subsequent fields, as in the accepted prototype and the existing dynamic shared menu. Do not reset the stored interval when hiding it. Preserve all existing action and interval choices and their menu order.

### Transpose

- Retain integer-cent storage and range **-2450 to +2450 cents**, inclusive; display semitones with two decimal places.
- An ordinary encoder step adds/subtracts **100 cents**. SHIFT + a step adds/subtracts **one cent**.
- Preserve the fractional tuning when making coarse changes: +1.23 becomes +2.23, not +2.00.
- Clamp at the existing limits without wrapping or overflow, including signed and larger encoder offsets.
- Reuse the shared decimal item's coarse/fine behavior, not the ordinary audio-clip item's data binding. Held SELECT may continue to enable fine editing where the shared decimal behavior already provides it.
- Author's call: keep Loop Station transpose unaccelerated so a physical detent remains one semitone or one cent as requested. Verify the upstream encoder path does not multiply SHIFT input. Add only the narrow per-item/shared extension needed; do not change acceleration for other menus.

### Copy To Track in horizontal mode

- Selecting the Copy To column does not copy anything.
- SELECT rotation chooses the destination directly. Initialize to the next track, wrap across tracks, and skip the source track.
- First SELECT press opens a review showing source and destination. A second, separate SELECT press confirms through the existing `copySettingsTo` path. Ignore button releases/repeats for confirmation.
- BACK in review cancels and returns to the Copy To field without copying; BACK outside review closes settings.
- While reviewing, instrument/paging buttons and pad presses do not confirm or change the target. An instrument-button re-selection must not execute the copy action through the generic shared menu's repeated-selection path.
- Successful copying changes settings only, preserving destination audio, name, output, effects and history. Busy/unavailable failures leave the destination unchanged and show existing feedback. Return to the Copy To field after success or failure; never claim success before checking the returned status.

## Compatibility and proposed author decisions

These are conservative implementation calls for approval, not additional requirements supplied by the user:

- Respect the existing **Horizontal Menus** preference. Use the new shared horizontal experience on OLED when enabled. Keep usable list-based behavior when disabled or on seven-segment hardware rather than forcing four invisible columns there.
- Retain the existing fallback list's SELECT-to-edit and Copy To Track flow. Give transpose the requested semitone/SHIFT-cent stepping while editing in that fallback as well. No other fallback navigation redesign is required.
- Reuse existing setting localization and add localized short labels/confirmation text only where needed. Do not adopt the prototype's English labels as firmware-only hardcoded strings.
- Keep live in-memory updates and normal Song Save/load persistence. Do not add per-turn SD writes, new song tags, enum renumbering or migrations.
- Keep the documentation preview marked as proposed until firmware integration is verified. The preview's helper buttons and SHIFT toggle are browser conveniences, not new hardware shortcuts. Keep its duration/audio limitations explicit.

## Component responsibilities and integration boundaries

1. **Loop Station settings host:** owns scope/track selection, opening/binding/failure handling, lifecycle, redraw requests, copy review and fallback compatibility. It supplies the current Loop Station model/context to its menu items.
2. **Loop Station menu items:** read/configure the existing Loop Station settings model; report relevance; expose native enum/numeric/decimal values and localized labels. Source, duration, playback, transpose and colour preserve their existing validation and command semantics. Read back authoritative state after commands; on rejection do not show an unapplied optimistic value.
3. **Shared horizontal infrastructure:** remains the only implementation of horizontal packing, button-to-column mapping, page navigation, rendering and LEDs. Make its host boundary explicit enough that Loop Station never depends on stale `soundEditor` clip, modulation, range, navigation or pad-shortcut state. Keep existing default behavior for existing callers.
4. **Prototype and documentation:** the accepted HTML remains a reference, not an alternative firmware UI engine. Once implementation is verified, update both feature guides and the demo's status/instructions to match what is actually supported.

Avoid duplicate configurable state and broad global context swaps. The builder may choose a small host interface or virtual hooks after examining all transitive `Submenu`/`MenuItem` calls; inheritance alone is not evidence that the path is safe. In particular audit initialization/relevance, begin/end session, child actions, redraws, item permission checks, decimal editing, `updatePadLights`, chain switching and global renderer use. Ensure menu/item lifetimes and parent pointers remain valid, including failed opens and returning to other editors.

## Acceptance and verification

- Production-path tests open global and each of eight track menus through their actual entry events, exercise all four instrument buttons, both paging buttons, direct SELECT edits and BACK/SONG exit behavior. Check empty columns, wrap, shorter-page selection and button releases.
- Verify every existing setting remains reachable. Exercise all four combinations of Start/Stop mode and preserve selection by field while visibility/page counts change.
- Test transpose coarse/fine changes in both directions, crossing zero, fractional preservation, exact range boundaries, large offsets and actual shared SHIFT press/release handling. Verify physical-offset handling is not accidentally accelerated/multiplied for this item.
- Test Copy To target wrapping/skipping source, no copy on selection/rotation/instrument re-selection, review confirmation, cancellation, failure feedback, settings-only effects and source/destination identity.
- Test live colour updates and save/load round trips without recolouring other tracks or changing audio. Preserve current duration/memory restrictions and command rejection behavior.
- Exercise the actual shared horizontal selection/rendering code from the Loop Station host, not just a probe with copied method bodies. Update probe dependencies or add a suitable shared-menu test seam as necessary.
- Use real OLED-canvas/font bounds checks for both menus, the last/conditional pages, long labels, negative/maximum transpose and copy review. Check feature-off and seven-segment fallback behavior.
- Run shared-menu regressions covering ordinary master/sample decimal editing, conditional items, accessibility highlighting, LEDs, horizontal chains and return to the sound editor. Build the affected firmware target.
- Browser-check the docs demo after final alignment; identify prototype-only conveniences and keep temporary edits/reset behavior clear.
- Device inspection remains separately required for readable 128-pixel OLED columns, real button LEDs, physical coarse/fine detents and uninterrupted playback while editing. Do not call host/browser checks hardware acceptance.

## Left to the builder

Choose exact hook/adapter names, file organization, concrete menu-item base classes and localized short labels; minimize unrelated shared-code changes. Determine the appropriate existing test/build commands and extend test support without replacing production behavior under test. Resolve any implementation-only details within the interaction, compatibility and isolation boundaries above.

## Builder check

A fresh embedded-firmware reviewer read this design and the relevant existing code on November 14, 2026, before handoff. Result: **nothing important left** to clarify before planning. The user subsequently approved the written design.
