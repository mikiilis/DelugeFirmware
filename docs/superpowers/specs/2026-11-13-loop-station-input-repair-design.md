# Loop Station input repair

Status: approved by the user on November 13, 2026. Hardware acceptance remains pending.

## Intent and evidence

Restore the already documented Loop Station controls. On the device, the eight track tiles appear but normal and Shift+centre presses appear inactive. SELECT opens global settings, but turning/clicking SELECT appears inactive; BACK closes settings.

Source investigation identifies three independent UI integration gaps:

- Normal pad commands do not call `SongModel::ensureBound`, while `SongModel::dispatch` rejects unbound Press, Release and Select commands.
- `LoopStationView::buttonAction` consumes otherwise unhandled buttons, including SHIFT. The shared button handler only calls `commandToggleShift` for unconsumed events, and pad gestures read that logical Shift state.
- Settings updates call `refreshDisplay`, which returns immediately for OLED. `uiNeedsRendering` schedules pad rendering, not OLED rendering. Unlike existing menus, these paths do not call `renderUIsForOled`.

These are source findings consistent with the reports, not completed device verification.

## Approaches

Selected: targeted UI wiring repair, as requested by the user. Preserve lazy backing-clip creation, shared Shift processing and the existing OLED rendering machinery. This avoids changing loop engine timing or gestures.

Alternative: eagerly create all eight backing clips on view entry. This adds unnecessary outputs and allocation pressure and does not solve Shift or OLED refresh, so it is not selected.

Alternative: rewrite input dispatch or the settings framework. This greatly broadens the change without addressing a demonstrated need, so it is not selected.

## Design

- Prepare an unbound track before an actionable press or selection from the Loop Station view, including encoder selection and RECORD where applicable. Ignore releases that have no prepared track; do not create clips on release. Keep allocation outside the audio boundary and defer allocation/menu-opening pad actions when the SD routine is locked. Report failed preparation with the existing track-unavailable popup rather than silently discarding the press.
- Let the shared handler process SHIFT press and release in both the performance view and settings overlay. Preserve all explicitly handled Loop Station buttons; do not introduce new global shortcuts.
- Schedule an OLED redraw when menu selection, edit mode, values or copy-target state changes. Retain the seven-segment display path. Refresh the performance OLED as needed for actions whose status it displays.
- Add regression coverage using production handler bodies, with only hardware dependencies stubbed. Cover fresh/unbound tracks, binding failures, Shift propagation, and OLED invalidation on encoder turns/clicks. Existing geometry/render tests alone do not exercise these paths.

## Scope and constraints

No loop-engine redesign, timing/default changes, new gestures, eager allocation of all eight tracks, or unrelated button behavior changes. Preserve existing lazy-binding transaction and retirement protections. Exact helper/test-harness organization is the implementer's call.

## Verification

Run new failing regressions before implementation, then the focused and existing Loop Station/OLED suites and an available firmware build. Device acceptance remains separate: centre starts recording, Shift+centre opens track settings, SELECT turns visibly move rows, clicks visibly enter/leave editing, and BACK returns normally. Do not claim hardware success from host tests or a firmware link.
