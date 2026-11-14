# Loop Station physical layout and ring colours

Status: implementation authorized by the user's November 14, 2026 instruction, "Implement this plan". Tasks 1–2 are implemented and task-reviewed; integration evidence is in `docs/superpowers/reports/2026-11-14-loop-station-layout.md`. Final integrated independent review and device acceptance remain pending.

## Intent

Make the device match `website/public/demos/loop-station.html`: tracks 1–4 across the physical top, tracks 5–8 across the bottom, and each progress ring starting at its own top-left corner and filling clockwise. Give fresh/default tracks eight distinct coloured rings without a white default that resembles the empty centre pads.

## Evidence

The HTML uses downward-increasing CSS rows and the clockwise ring sequence beginning `[0,0], [1,0], [2,0], [3,0]`. Hardware coordinates instead increase upward: `y=0` is bottom and `y=7` is top, documented in `website/src/components/deluge_companion/data/control_coordinates.ts:7-8`.

`src/deluge/loop_station/geometry.cpp` currently applies the HTML-style row arithmetic directly to hardware coordinates. Thus physical top-left addresses slot 4 (magenta/pink), not slot 0 (yellow), and ring progress is vertically reflected. The second tile on the physical top addresses slot 5, whose default is White. Sidebar input/rendering also assumes `y==0` means the top. Existing geometry and UI tests encode these same mistaken coordinates.

## Approaches

Recommended: correct the physical-to-visual transform once in the shared Loop Station geometry, and correct both sidebar input and drawing to the physical top row. This keeps hit testing, colours, progress and hold-to-clear sweeps consistent.

Rejected: reverse only the renderer's animation index. It would leave track order, interaction mapping and sidebar position wrong. Rejected: invert global pad/PIC coordinates, because other views already use the hardware convention.

## Design

- `padAddress(x,y)` continues accepting physical coordinates. Validate bounds first, then calculate visual row `7-y`; use that row for both slot and local ring coordinates. Do not invert coordinates again at its callers.
- Physical top row contains slots 0–3; bottom contains slots 4–7. Within every tile, the 12 border pads advance top edge left-to-right, right edge downward, bottom edge right-to-left, left edge upward. The centre remains 2×2. The shared ordering also applies to clear sweeps.
- Start All and Stop All sidebar pads are at `(16,7)` and `(17,7)` in input and rendering. Bottom sidebar pads become inactive.
- Keep stable slot IDs, backing clips, audio, output routing, names and saved settings attached to their existing logical track. Only their physical tile location changes; do not shuffle saved tracks or audio to preserve the old incorrect display.
- Proposed defaults, in logical track order: Yellow, Cyan, Blue, Purple, Magenta, Pink, Gold, Lime. Replace only slot 5's White default. Render Loop Station Pink as saturated `RGB(255,0,128)` locally, rather than its current pale `pink_full`; do not modify the global palette. Update the demo's sixth colour to `#ff0080`, leaving its correct layout/animation unchanged.
- Each track must expose **Colour** in its own configuration: **SHIFT + track centre → Colour → press SELECT to edit → turn SELECT to choose → press SELECT to finish → BACK**. Reuse the existing `MenuField::Colour` and 11-entry track palette, including White; do not add a second colour picker or arbitrary RGB editor. Apply the choice immediately to that track's identity ring, retaining centre/state/error indications and other tracks' settings/audio. Persist chosen colours through the normal Song Save/load workflow, not an SD write on every encoder turn. Returning with BACK retains the in-memory choice.
- Preserve centre state colours and existing recording/error/pending colour overrides. Ring uniqueness applies to the default identity palette, not to temporary state/error indications.

## Accepted planning assumptions

The palette adjustment above was a proposed implementation choice, not an exact palette supplied by the user; it was accepted with the November 14 implementation authorization. Distinct defaults remain editable. Preserve explicitly saved/custom colours—including White—and permit duplicate user-selected/copied colours; do not silently recolour existing songs or introduce a persistence migration. Consequently an existing song with saved White will remain White until edited. Enforcing uniqueness for existing songs would require a separately authorized design change.

## Constraints and non-goals

No recording/timing/gesture/engine changes, no schema or enum renumbering, no global coordinate inversion, no forced recolouring of saved songs, and no unrelated input-edge fixes. Keep previous lazy binding, Shift, SD deferral and OLED updates intact. No new dependencies.

## Acceptance

Tests must verify independent physical-coordinate tables for all eight tiles, all 12 ring steps, frame wrap, clear direction, and top-row-only sidebar actions. Exercise the actual pad renderers as well as geometry: at phase zero, physical top-left of slot 0 must be Yellow and slot 4 must occupy bottom-left. Verify default RGBs remain unique/chromatic after dimming and saved White/slot identities round-trip unchanged. For each of the eight tracks, test entering its configuration, navigating/editing Colour, visible ring updates, palette wrapping, BACK retaining the choice, other tracks remaining unchanged, and colour save/load restoration. Device inspection is still required to establish perceived colour separation and actual LED orientation; host tests do not establish hardware contrast.
