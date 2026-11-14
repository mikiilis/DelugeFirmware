# Loop Station selected-track indicator and ring pulse

Status: approved design and HTML preview; firmware implemented and host-tested, pending physical-device inspection. Supersedes only the deferred ring-pulse scope of the November 14 settings-UI plan; does not reopen its completed work.

## Intent

After tapping a track's ring, make it immediately clear **which track is selected** on both the OLED and the physical ring pads. The user chose a pulse, not flicker, and wants to see it in `website/public/demos/loop-station.html` before firmware integration.

## Goals and boundaries

- Preview first: adapt the existing interactive, silent HTML demo in place. On border selection, show an explicit selected-track number on its mock OLED and pulse only the selected track's twelve border pads in its configured colour. Keep the existing progress animation, settings interactions, Pause animation and reduced-motion behavior. Clearly label the demo as illustrative until firmware verification.
- After the user reviews the preview, mirror the approved behaviour in firmware: show the selected track number prominently on the Loop Station root OLED without losing memory, save and undo status; pulse the selected ring whether stopped, empty or playing. Encoder selection should give the same feedback as border selection. When track settings are foreground, use the addressed settings track as the visual focus without changing engine selection; global settings use the selected engine track. Restore ordinary selected-track focus on closing settings.
- Pulse brightness, not hue or loop position. Keep filled vs unfilled progress readable throughout the cycle, leave centres and other tracks unchanged, and let error, recording/overdub, pending-action and hold-to-clear indications win. Reuse the existing record/overdub pulse speed and phase rather than the earlier proposed ~1.5-second cycle; the preview feeds the same pulse value to selection brightness, while firmware should follow its existing record/overdub visual clock.
- No changes to audio, transport, recording gestures, storage, track identity or allocation behavior. Do not replace the native settings UI or introduce a second rendering loop. Existing seven-segment fallback must not regress; the OLED-specific label is not a promise of new seven-segment UI.

## Current state and constraints

`LoopStationView::padAction` dispatches `Select` on border presses and redraws. `renderOLED` currently draws a static title plus memory, saved/unsaved and undo rows. `renderMainPads` renders each track's configured colour and clockwise phrase progress, with state indications taking priority; it does not inspect selection. Settings are a separate foreground `RootUI` forwarding pads from the root, so continuous animation there needs pad-only redraw scheduling that does not overwrite menu LEDs or OLED. The existing HTML demo already shows `TRACK n` and a thin selected-pad border but has no selected-ring brightness pulse; its `animate` loop is the natural preview seam.

## Approaches

1. **Chosen: preview in existing HTML first, then implement the same visual rules through the firmware's current view/menu rendering paths.** Lets the user judge the appearance before hardware work; requires care with foreground-menu redraw scheduling and real OLED width.
2. **Not chosen: firmware first and sync the demo afterward.** Less risk of prototype/firmware divergence during development, but denies the requested early visual review.
3. **Rejected: change engine track state or start a new audio timing task to drive the pulse.** Unnecessary musical side effects and avoidable real-time complexity; visual timing and focus belong to the UI.

## Review and acceptance

1. Provide an interactive revision of `loop-station.html` as the **first implementation deliverable** and obtain the user's visual sign-off before proceeding to firmware. The design document and implementation plan themselves are not the preview. The initial proposed mock OLED text is `LOOP STATION · TRACK 3` or an equivalently legible compact selected-track label, subject to real-display bounds; preserve the other status rows.
2. Test border and encoder selection across all eight tracks; demo settings focus, custom/duplicate/white colours, Pause animation and reduced motion. Verify that the visual effect does not change centre pads, other rings, clockwise progress or priority indications.
3. Exercise real production-path pad and OLED rendering, including partial-row/canvas bounds, stopped transport and foreground settings ticks. Test that the phase continues across selection/menu changes and that neither settings LEDs/OLED nor audio/transport state are disturbed. Do not claim physical appearance proven by host or browser tests: inspect brightness and legibility on device.

## Left to implementation planning

Determine exact OLED wording/layout against actual font bounds, the minimal pad-only foreground redraw hook, integer pulse brightness levels that preserve progress contrast at the existing record/overdub visual rate, and whether any demo-specific accessibility alternative is needed beyond its existing Pause/reduced-motion controls. Stage the demo review as a hard gate before firmware tasks.
