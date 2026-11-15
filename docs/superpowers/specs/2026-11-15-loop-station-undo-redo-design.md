# Loop Station layer undo and redo

Status: Approved by the user on 2026-11-15 after a fresh builder check found no important unresolved questions. Implementation awaits plan review and execution-mode selection.

## Intent

Let a performer remove the selected track's most recent recording layer and restore it without affecting other tracks. Require a second matching button press before either operation changes audio.

User requirements:

- BACK/UNDO requests Undo on the selected track.
- SHIFT + BACK/UNDO requests Redo on the selected track.
- A second press of the same shortcut confirms the requested operation.
- Undo removes the latest overdub, or the initial recording when there are no remaining overdubs.
- Undo is allowed during active recording/overdubbing. The confirmation press closes capture and removes that recording/overdub; the first press does not interrupt capture.
- Use unified history, extending the existing history engine rather than introducing a separate initial-take stash.

## Scope and constraints

This is a cross-cutting firmware change: history, resident audio/page ownership, engine state, Song model orchestration, view input/feedback, tests, and the feature guide. Preserve the existing RAM-resident audio architecture and bounded history budget. No SD work, allocation, phrase-length scans, or unbounded copies may be added to the audio callback. Publish state changes at the existing serialized audio boundary and prepare replacement audio in bounded foreground work.

Non-goals: undoing settings/FX, selection, transport, or explicit hold-to-clear; independent stem editing; unlimited history; persistent undo history across Song loads; changes to the browser demo; redesigning the main screen. Explicit Clear remains destructive and discards that track's history. Track identity, binding, output, FX, and settings survive Undo to empty.

Unacceptable: first-press audio changes; removing more than one layer per confirmation; removing the initial recording because an overdub's history is unavailable; stale confirmation operating on another track or Song; falsely reporting success; stopping other tracks; redoing an invalidated branch.

## Approaches

1. **Unified history — selected by the user.** Add initial-recording/empty transitions to the existing ordered per-track history. Continue using page before-images for overdubs and retained versions for Redo. Costs additional work on pool/audio/engine transitions but gives one branching and eviction policy.
2. **Separate initial-take retention — not selected.** Leave overdub history intact and add a model-owned initial-recording stash. Smaller initial history changes, but duplicated cancellation, lifetime, and branching logic risks disagreement between the two mechanisms.

## Layer semantics

A layer is one continuous capture session: opening an overdub gate through closing it. Several loop passes during one open gate are one layer, not separate layers. Existing mandatory provisional double-tap cancellation is not an extra layer and retains its current behavior.

For a retained sequence `empty -> initial -> overdub A -> overdub B`, successive confirmed Undo operations yield `initial+A`, `initial`, and `empty`. Successive confirmed Redo operations traverse those states forward. Every step requires its own two presses. A held BACK button cannot repeatedly confirm; a release must separate the two press edges.

History remains bounded. Missing/incomplete history fences navigation: do not skip a lost overdub and remove an older layer or the initial recording. Exhausted or unavailable history is reported honestly. Recording/overdubbing must remain possible when history cannot be retained.

Builder call proposed for approval: a successfully loaded checkpoint is the base recording of a new volatile history chain. It may be undone to empty and redone as one whole recording, but pre-save overdubs cannot be individually recovered after load. No new persisted history schema is introduced.

## Confirmation interaction

The root Loop Station view owns these shortcuts only when the existing shared-exclusive-mode policy permits it. Menus retain BACK navigation and shared modes retain their existing input ownership. The shortcut always uses track selection, regardless of AFFECT ENTIRE.

States are Idle, ConfirmUndo(track, identity), ConfirmRedo(track, identity), and Applying(track, request). From Idle, a valid first press arms the operation and shows its track and instruction. A matching second press requests the operation once and clears confirmation. If unavailable, show a concise reason and do not arm. Selecting the opposite shortcut replaces the pending prompt with the opposite operation only if eligible; it never confirms the previous operation.

Modifier press/release edges alone do not cancel a prompt: SHIFT must be usable between Redo presses. The second BACK press must have the requested modifier state. There is no rapid-double-click requirement and no time-based confirmation expiry; the visible prompt lasts until confirmation, cancellation, or loss of eligibility.

Cancel confirmation on track selection changes, pad actions, unrelated control actions, opening a menu/another UI, leaving Loop Station, transport changes, Song/model replacement, unbinding/deletion, explicit clear, or a change to the target recording/capture-session identity. Automatic completion of the target capture cancels the prompt rather than changing what its confirmation would remove. Do not cancel solely because more frames arrive within the same active capture session. Button release edges and SHIFT alone do not count as unrelated control actions.

Prompts must be tied to stable Song/model, slot/binding, phrase, and capture-session/history identities, not just the selected slot number. Revalidate on confirmation. A stale prompt cancels with no audio change; the user must request again.

Proposed feedback, adapted to display width: `TRACK 1: UNDO? / BACK TO CONFIRM`, `TRACK 1: REDO? / SHIFT+BACK TO CONFIRM`; during work `UNDOING`/`REDOING`; then `UNDONE`/`REDONE`, or `NO UNDO`, `NO REDO`, `BUSY`, `NO MEMORY`, or an appropriate failure message. The confirmation must remain visible on OLED and seven-segment hardware despite periodic graphics refresh. Use existing display conventions and localization patterns; exact wording is a builder call.

Update the BACK indicator for the selected track's eligible operation (Undo normally, Redo with SHIFT), including a removable active capture. Never imply unavailable history is usable. No track selected consumes the shortcut without invoking unrelated global Undo; give concise no-track feedback.

## Active capture

Only confirmed Undo closes capture. Capture continues normally while confirmation is pending.

- **Active overdub:** close exactly that capture session, settling its mandatory provisional state without invoking the centre-pad double-tap gesture, and undo its captured changes. Keep the backing phrase playing if it was playing. Redo restores the additions captured through the confirmation boundary.
- **Active initial take:** seal the frames captured through confirmation without entering the normal automatic Play/Dub continuation, retain the result for Redo, and transition the track to empty/stopped. A zero-frame take simply returns to empty and creates no Redo entry.
- **No samples changed in an active overdub:** close the gate, report no layer to undo, and do not fall through to undo an older layer.
- **Redo while capturing:** unavailable. Opening a new recording/overdub invalidates that track's Redo branch under the existing new-edit policy; do not interrupt capture to redo an older branch.

Confirmed capture closure is immediate at the serialized boundary and bypasses quantized Start/Stop settings; layer restoration may finish asynchronously. Cancel obsolete track gesture/deadline state so a later pad release or queued completion cannot restart capture or clear restored audio. Other tracks and Song transport are unaffected.

Check availability and resource admission before closing capture wherever possible. If a restore subsequently fails after capture was closed, preserve the cumulative captured audio, leave capture closed, and report the failure. Never substitute an older undo step, discard audio to make room, or report the requested removal as successful.

## Unified history and ownership

Extend history to distinguish initial-recording boundaries from overdub boundaries and to represent an empty current state explicitly. Register an initial boundary only for a completed, nonzero recording, including a load-admitted base checkpoint under the proposed rule above. Continue coalescing unchanged boundaries and respecting incomplete-history fences.

Initial Undo must detach and retain the recording for Redo, not retire it through `ResidentAudio::clear`. Extend PagePool ownership operations so a live sealed phrase can become retained/detached with no active phrase, then be republished without copying the entire recording. Extend ResidentAudio and Engine transitions to agree that the track is truly empty: no backing playback, capture, phrase length, pending actions, or stale gesture/completion state. Keep its clip bound and track selected.

Redo from empty republishes the retained phrase and restores normal eligibility for overdub/initial-boundary Undo. Builder call proposed for approval: it restores **stopped**, starting from zero when the user next starts it; it does not unexpectedly resume playback. For ordinary overdub Undo/Redo, preserve the current playing/stopped state and phase modulo the unchanged phrase length, as the existing restore path does.

Starting a new recording in an undone-empty track discards its old Redo branch and starts a fresh chain. Starting an overdub after Undo discards that track's forward branch. An edit to another track does not invalidate this track's Redo. Explicit clear, binding deletion, replacement, and model destruction cancel pending jobs and release retained versions through bounded retirement.

Retained initial recordings consume existing bounded audio/history resources, not a second unbounded pool. Under pressure, a whole unreachable/evicted history range is retired safely and its navigation becomes unavailable. Live audio, admitted capture, rollback protection, and snapshots must never be reclaimed as history. Redo references must remain owned until republished or retired, even across save snapshots and archive activity.

## Model orchestration and asynchronous lifetime

Keep history navigation in SongModel rather than implementing audio edits in the view. Provide a model-level confirmed operation that revalidates identity/eligibility and serializes active-capture closure with creation of the correct restore job. `canUndo`/`canRedo` must describe model-level eligibility, including active-take removal and empty-state Redo, not merely expose the current nonempty History queries.

Ensure accepted history work has a regularly scheduled, bounded foreground owner. The current view calls restore service only from `timerCallback`, with no timer scheduling in this view; do not assume that service is live. Prefer Song/model-owned foreground servicing alongside the existing lifecycle, so a confirmed operation continues safely if a menu opens or the root view changes. The view observes completion rather than being the sole owner of an accepted job. Keep checkpoint restore and user history navigation serialized, and avoid double service/commit.

Reject repeated requests while a restore is applying; do not queue button presses. New capture on the same track invalidates/cancels an in-flight navigation, and stale work cannot publish afterward. Operations on another track must not be blocked except that the existing single restore job may report Busy for another history request. Retain a job on commit Busy and retry it; do not drop its ID or claim success. Handle all terminal statuses, cancellation, Song replacement, view reentry, and release without leaving stale UI or model ownership behind.

Builder calls: exact API names, fixed-capacity descriptor layout, identity-token representation, foreground work budget, and reusable input-state helper. Do not increase the reserved memory profile without a separate need and explanation. Preserve sample-accurate publication and existing transposition/FX ownership invariants.

## Persistence

Undo/Redo changes audio content revision and dirty status. Saving an undone-empty track must omit its old audio checkpoint while retaining settings/binding; loading that save must not resurrect the removed recording. A save may keep its existing immutable snapshot if it began before Undo; it must not silently mark a later state saved. Saving does not discard volatile Redo or make that Redo durable. Loading starts a new volatile history chain and never restores hidden, undone recordings. Test interaction with a retained initial recording, a save in progress, and Redo after save.

## Verification and acceptance

Use host tests with exact stereo frame assertions, not UI state alone:

- Multiple overdubs unwind and redo in order, including multiwrap capture sessions and final initial-to-empty Undo.
- Initial Undo/Redo retain audio, binding/settings, and the specified stopped-on-initial-Redo behavior; zero-frame capture is harmless.
- Active initial/overdub Undo first press leaves capture untouched; confirmation removes only the target session and leaves other tracks unaffected.
- Mandatory provisional rollback, pending deadlines/gestures, and natural capture completion cannot turn confirmation into a different action.
- Two released button presses are required per step; SHIFT routes Redo; changing modifiers alone does not cancel; cancellation and stale identity checks work.
- No-selection, missing history, incomplete fences, memory pressure, branch invalidation, concurrent archive/snapshot, and clear/delete/load cases preserve live audio and bounded ownership.
- Real root-view input tests exercise the shortcut and feedback on both display paths; confirm shared menus/modes do not lose BACK ownership.
- Foreground servicing progresses without a view timer, survives UI handoff, retries commit Busy, reports terminal failures, and cannot publish canceled work.
- Save/load after Undo to empty and after Redo reflect the correct committed audio; loaded checkpoints behave as the proposed base-recording boundary.
- Run relevant existing Loop Station suites and allocation guards, firmware Debug build, and available OLED tests. Record precisely which checks run and their outcomes.

Hardware acceptance remains separate: audible layer removal/restoration, capture-boundary behavior, playback phase/no clicks, display legibility, confirmation ergonomics, BACK LED behavior, and memory/DSP deadlines require physical Deluge testing. Do not label host tests as hardware validation.
