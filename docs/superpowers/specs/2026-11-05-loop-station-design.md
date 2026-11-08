# RAM-resident Loop Station design

Status: approved by the user on November 8, 2026. Implementation follows the matching plan in `docs/superpowers/plans/2026-11-05-loop-station.md`; design approval does not constitute hardware acceptance.

## Intent and decisions

Build a clean eight-track performance looper in DelugeFirmware, using the supplied Loop Station Algorithm Specification as the behavioral baseline, except where this design explicitly supersedes it. Do not port the earlier branch's implementation.

User decisions:

- Immediate repeated overdubbing without waiting for storage is the primary requirement.
- Active loops reside in RAM.
- The target shared active-audio budget is 32 MiB, giving roughly twelve seconds per track when divided equally. The user accepts ten seconds per track if measurements show that extra operating memory is necessary; the bounded fallback below replaces the earlier unconditional 32 MiB requirement.
- Undo availability may temporarily decrease when SD storage falls behind, so overdubbing can continue.
- Progress animation uses the filling ring selected by the user on November 8, 2026: twelve border pads fill clockwise from physical top-left, then reset on each loop wrap.
- The user accepted the eight-tile layout and centre-pad record/play/overdub cycle, double-tap stop, two-second stopped hold-and-release clear, border selection, and sidebar Start All / Stop All controls.
- An unmodified CROSS-SCREEN press in Song/Session View opens Loop Station. CROSS-SCREEN or SONG returns to Song/Session View. The CROSS-SCREEN LED is lit at the Loop Station root; existing clip and Arranger uses remain intact.
- For optional ONE_SHOT playback, an active overdub repeats so layers can be added. Finishing overdub with the centre pad closes capture immediately, finishes the current phrase pass, and stops. A subsequent start plays the phrase once. Ordinary LOOP playback continues repeating after overdub ends.

This is a substantial audio-engine and storage project. Hardware feasibility and measured budgets precede the complete performance UI. No claim of eight-track hardware acceptance is made here.

## Goals, non-goals, and failure conditions

Goals: eight independent phrases, immediate continuous additive overdubs, stable sample-frame phase, dry capture, native-speed playback, ordinary track FX, independent source/monitor settings, responsive pad gestures, asynchronous persistence, and visible honest resource/history status.

Non-goals: unlimited active duration, unlimited volatile history, guaranteed operation with arbitrary FX/synth load, durable recording without functioning storage, a Boss feature-for-feature clone, or importing unpublished branch XML formats. Ordinary songs and recording workflows must remain compatible.

Unacceptable: allocating or accessing SD in the per-frame mixer; stopping or refusing an admitted overdub solely because history storage is slow; silently changing the current phrase on SD failure; evicting live loop memory; creating unbounded snapshot/dependency queues; reporting unsaved audio as saved.

## Approaches

1. Complete cumulative WAVs streamed during capture: compatible with ordinary files, but a short overdub on a long phrase creates full-phrase copying and dependent-generation backlog. Rejected as the live representation.
2. Shared immutable on-disk blocks: avoids copying unchanged regions, but requires a custom persistent representation and still exposes live long phrases to card-read stalls. Not selected.
3. RAM-resident active phrases with asynchronous bounded history/persistence: decouples live performance from SD throughput, at the cost of a finite phrase pool and reduced history under pressure. Selected by the user.

Durable revisions can still be ordinary cumulative WAVs. They are checkpoints, not prerequisites for live state changes.

## Memory and admission

Builder proposal: use interleaved stereo signed 32-bit firmware-native samples for every active phrase, including mono physical sources. Mono routing feeds both channels; overdubs may subsequently use stereo sources. Specify saturation and gain in the firmware's actual sample convention, not generic full-scale int32 assumptions.

32 MiB / (44,100 frames/s * 8 bytes/frame) gives approximately 95.11 seconds of combined active audio, before page rounding. Eight equal phrases have about 11.89 seconds each. This is one pool, not eight fixed partitions. A live phrase has a positive immutable frame length; subsequent passes update the same storage.

Reserve the entire selected active pool before enabling live operation, from external memory with explicit non-stealable lifetime ownership. Do not assume the ordinary small external allocator can supply it. The memory integration must leave an independently measured reserve for normal sample streaming, outputs, FX, menus, archive staging, and history. Prefer 32 MiB. If measurements demonstrate that the required reserves cannot coexist with it, the user authorizes a reduced pool that still fits eight independent ten-second stereo phrases, including per-phrase page rounding. With the candidate 4096-byte/512-frame pages this requires 6896 pages, or 26.9375 MiB. This is a measured operating profile, not an unrestricted fallback to any allocation that happens to succeed. Document the selected capacity, reserve measurements and reason for reducing it, and show the reduced capacity when enabled. If neither profile fits safely, report enable/admission failure.

Builder call: retain a shared pool, allowing different phrase lengths rather than fixed per-track partitions. The ten-second fallback capacity is evaluated with eight equal phrases in an empty pool; later admission depends on existing use. Select and reserve the operating profile in safe foreground setup while no live phrases own pages. Never shrink a live pool, truncate an admitted phrase, or change profiles in the audio callback. The device-wide runtime flag remains default Off.

Builder proposal: fixed-size preallocated pages with a free-page index, no compaction while performing. All page tables and free-list capacity are provisioned in setup. Empty-track capture claims available pages incrementally using bounded constant-time operations; no general allocation is permitted in its sample path. Page exhaustion seals the valid initial phrase at the exact capacity boundary and enters Play/Dub according to Record Action, with an explicit memory-limit notice. A fixed-duration take that cannot fit is rejected before starting. Simultaneous manual takes share remaining pages in a deterministic fair order. Clearing returns pages only after readers release them.

Existing loops always retain their admitted pages; overdubbing never requires another active-phrase allocation. Audio callbacks have a bounded maximum amount of work regardless of phrase length or history depth.

## Ownership and component boundaries

- `loop_station` engine: firmware-independent gestures, timing, state, routing decisions, generation identities, geometry and bounded page/history policy. No filesystem, UI, physical pad scanning or SampleRecorder ownership.
- Song adapter: settings, eight slot bindings, durable history manifests and live audio ownership.
- Audio renderer/capture adapter: frame-indexed playback and in-place dry cumulative mixing into reserved pages; integrated with normal AudioOutput FX.
- Archive service: stable-snapshot reads, WAV writing, finalization, manifests, restore staging and cleanup, all outside realtime processing.
- LoopStationView: pad gestures, OLED settings, LEDs and nonblocking status.

The audio engine services musical deadlines and sample-clock actions. Graphics never executes queued transport changes. Archive callbacks use generation IDs and lifetime handles, never unvalidated view/clip pointers.

Ordinary SampleRecorder users remain independent. Loop Station live capture does not create retained SampleRecorders for each session or pass. The original 16 recorder-object/16 capture-unit/eight-cluster limits therefore do not define this mode's live admission. Shared global resources still require accounting where actually used; do not introduce overlapping fictional recorder charges.

## Realtime audio

Each slot owns phrase frames, playback frame, capture gate, and current live generation. Read the dry backing frame, save any required before-image, add the aligned physical input with saturation, and advance/wrap. Multiple wraps in one callback preserve earlier additions. Overdub start and stop take effect at their dispatched sample boundary, independently of storage and Start/Stop Action.

An initial take establishes logical frame zero. All RAM generations store logical frame zero at index zero: rotated WAV storage and sampleZeroFrames are unnecessary for newly recorded RAM loops. Playback/retrigger/one-shot completion are sample-based; tempo changes affect only musical deadlines and metadata. No crop, resample or time-stretch is applied to recorded content.

Capture branches before transpose, FX, level and pan. The audible signal is the dry backing plus the monitor signal, once each. Ensure newly added input is not accidentally heard twice from both mutated backing and monitor in the same render. Monitoring Off does not disable capture or playback transpose.

Input latency compensation and monitor delay must be measured and explicitly budgeted. No input after a closed gate belongs to an overdub; alignment must not sneak in a post-stop capture tail. Preserve captured samples. Builder proposal: short fixed, bounded playback transition ramps/crossfades suppress boundary clicks without mutating phrase content, changing length or delaying the capture gate; choose and document sample count through listening tests. Arbitrary discontinuous source audio cannot be promised perceptually seamless under every condition.

For multiple captures sharing a physical source, each enabled capturing output receives its own monitor return through its own FX; selected-idle monitoring is suppressed for that source. This can deliberately sum multiple returns; no hidden normalization is applied. Document the headroom implication. Physical sources remain LEFT, RIGHT, STEREO and BALANCED.

## Bounded undo and stable snapshots

Builder proposal: a separate preallocated before-image page pool, outside the selected active pool. Its exact safe size is determined by the memory audit and hardware feasibility milestone, not assumed to be another complete active pool. History metadata and snapshot descriptor counts also have explicit caps.

At session start, open a logical undo boundary. On the first modification of a page during that session, retain its previous contents if history space allows. Later passes in the same session do not create additional undo boundaries. A held before-image is immutable. The audio path performs only bounded page copies and fixed-capacity bookkeeping; measure worst-case simultaneous page-boundary cost. Split copying across bounded spans if whole-page copies violate deadlines.

On pressure, invalidate the oldest non-durable history entries and cancel expendable archive snapshots until buffers are available. If that cannot preserve the current session's full before-image, mark that session non-undoable and continue capture. Never advertise an undo entry whose complete prior state cannot be reconstructed. A non-undoable edit breaks the traversable volatile chain; old durable checkpoints remain explicitly identifiable rather than masquerading as the immediately preceding step. Coalesce repeated warnings into an OLED/history indicator.

Archive snapshots use page-version handles and the same bounded preservation rules: a writer must see one coherent generation, never a mixture of generations from successive passes. A snapshot can pin current pages; before those pages are overwritten their snapshot contents must be preserved in reserved storage. If preservation cannot fit, cancel that uncommitted snapshot, discard its partial output safely, and let audio proceed. No unbounded copy-on-write ancestry. One active archive job per slot, fixed global staging buffers, and at most one replaceable checkpoint request per slot. Catch-up coalesces superseded generations rather than promising to persist every session.

Progress toward durability is guaranteed only when enough storage bandwidth and snapshot memory become available, for example after overdubbing pauses. Under continuous edits and stalled storage, live audio continues but history and archive progress may be lost. Expose unsaved status throughout.

Undo/redo is available only for complete retained states. Restoring on-disk history may load asynchronously and may show History Loading; immediate history navigation is not the priority guarantee. Prepare a replacement using bounded staging before an atomic audio-boundary switch. If staging cannot fit, reject history navigation without changing live audio. New overdubs cancel pending restores. Redo becomes inaccessible on a new edit; old disk branches are not physically deleted until a replacement manifest is durable. History never changes a playing phrase's length/phase implicitly: restoration translates phase modulo the restored phrase length, while stopped starts use zero.

## Storage, saves and faults

Export successful stable checkpoints as complete cumulative 24-bit 44.1 kHz WAVs, retaining 32-bit working precision during live passes. Reloading quantizes to the WAV format; do not claim a bit-identical 32-bit round trip. Persist frame lengths, slot bindings, generation IDs and available durable history. A revision is durable only after all writes, header update, close/sync and manifest publication succeed. Design crash recovery around the actual FAT APIs; interrupted partial files must not become referenced revisions. No unconditional power-loss guarantee for commodity SD media.

SD absence, full card, write protection or write failure affects persistence, not admitted RAM playback/capture. Keep current live audio, mark unsaved/archive error, and retain the last durable files. This explicitly replaces the supplied spec's rollback-on-commit-failure behavior. Acknowledging a storage error never reverts or stops a live phrase. Input/resource errors remain slot-local and distinct from storage errors.

Save requests a consistent snapshot and remains visibly pending until that snapshot and its song manifest complete. Later live edits remain unsaved. If a needed snapshot is cancelled under pressure, Save reports incomplete rather than success. Loading/replacing a song, disabling the mode, and releasing the pool may discard volatile audio: use the project's normal explicit unsaved-change handling, and never wait invisibly for an impossible archive drain. Leaving the view or stopping transport retains RAM audio and does not require SD publication.

On load, check summed decoded RAM demand before binding; hydrate admitted loops outside the audio callback. Oversized imported material is left as ordinary audio clips with a clear unavailable-slot notice, never silently truncated. Bound resident clips use the RAM provider even when LoopStationView is closed, including Song View playback. Do not let an ordinary WAV voice play the same bound phrase in parallel. Clear retains backing clip/output/settings; deleting the backing clip releases the binding and audio only after references drain.

## Performance UI and settings retained from the supplied specification

Eight 4x4 tiles form a 4x2 layout; tracks 1-4 are physical top row, 5-8 bottom row. Inner 2x2 is the action button, outer twelve pads are selection/progress. Keep the specified state colours, progress colours, stopped hold-to-clear sweep, 350 ms double press, two-second stopped hold, SHIFT-centre track settings and SELECT global settings. Border selects the FX/monitor target. Display root title Loop Station plus concise memory, unsaved and history-pressure status when relevant.

Keep the default-Off community flag. From the Song/Session root, with no modal operation or held-pad edit active, an unmodified CROSS-SCREEN press opens Loop Station after memory preflight; consume its paired release so it cannot immediately toggle back. If the feature is disabled, show the enable hint. At the Loop Station root, an unmodified CROSS-SCREEN or SONG press returns to Song/Session View through the normal exit policy. Keep the CROSS-SCREEN LED steadily lit at the Loop Station root and restore each destination view's own indicator state on exit; menus retain their existing navigation behavior. Display the root title Loop Station. KEYBOARD keeps its existing Performance View shortcut in Song/Session View. Do not intercept clip wrap-edit, Arranger auto-scroll or menu paging, and do not switch/stop active Arrangement playback to enter the mode; reject entry with a concise notice in that situation. Entry itself never starts recording or transport. Bound clips are lazily created with independent outputs and LOOP 1-8 names.

Keep Record Action, song-wide Start/Stop Action with strictly future straight/triplet pulses, Q16 fractional deadlines rounded up to sample boundaries, pending cancellation independent of fixed-duration recording, common group deadlines, and one-shot semantics. Initial capture completion enters Play/Dub immediately without storage or a second start deadline. Double tap discards its provisional overdub via mandatory protected before-images described below; normal history availability cannot weaken this gesture.

Provisional double-tap rollback is mandatory gesture state, not expendable history. Reserve a dedicated before-image buffer per track for up to 350 ms of distinct stereo frames (15,435 frames at 44.1 kHz, plus dispatch/alignment padding). Save each original frame only on its first provisional modification; short-loop revisits use the same saved original. These buffers are outside the active pool, cannot be evicted by history/archive pressure, and are allocated before operation. Prefer this uniform path over choosing a fallback after capture has begun. Rollback switches the audible provider to the original frame mapping at an audio boundary and restores active pages using bounded work, without SD; prove safe reuse of the scratch buffer before another provisional session. A complete bounded rollback/reuse strategy is part of the hardware feasibility gate. Expiry confirms the additions; normal session undo may still degrade independently.

Snapshot jobs must not admit provisional additions. An existing snapshot stays pinned to its earlier generation; a checkpoint or Save request involving a provisional track waits for gesture resolution and then selects the confirmed or rolled-back generation. It cannot report completion with discarded input. This short gesture-resolution wait affects Save only, never live overdub onset.

Stop All covers recording, overdubbing and playback: close overdub gates immediately and apply Stop Action to backing playback/initial-take completion. Transport stop closes captures and stops audio immediately; leaving the view applies the supplied Stop Action to initial takes, cancels queued starts and closes overdub gates. RAM references remain valid without blocking for archive jobs. REC indicates actual open capture gates, not background archive work. BACK indicates usable undo/redo, not merely files on disk.

Keep physical input, monitoring, duration, playback mode, progress colour and transpose settings. Copy To Track copies only exposed loop configuration (source, monitoring, duration, colour, playback and transpose); it preserves destination audio, identity, output/FX, history and state. Reject while its destination is capturing or undergoing a live-state replacement. Remove obsolete sync/start/stop/fade fields from this operation and new saves. Ordinary backing-clip menus retain their normal semantics except sample replacement/reverse/stretch operations that conflict with bound RAM ownership: these require an explicit stop/unbind path rather than silently changing the resident phrase.

## Transpose

One combined pre-FX stage processes audible backing and permitted monitor input. Unity bypass is exact; dry recordings remain unchanged. Provision shifter state outside the callback, retain it for the bound output, and reset on bypass exit/discontinuity, not ordinary pitch edits. Native phrase phase is independent of pitch ratio.

The supplied LivePitchShifter description is not a statement about current code: this checkout uses a 16-sample interpolation maximum and disables its experimental repitched buffer. Inspect/adapt the production algorithm or implement a bounded per-track adapter; do not enable experimental macros as a shortcut. Stereo independent input histories, latency, reset behavior and eight-track CPU cost require explicit validation. Range remains +/-24.5 semitones. Extreme transpose/FX acceptance is measured separately from base RAM looping.

## Persistence compatibility

Introduce a versioned RAM-loop schema. Preserve ordinary Song XML and the supplied new Start/Stop Action IDs/defaults. Do not implement migration for an unpublished Loop Station branch unless representative files and a concrete compatibility requirement are supplied. Ignore obsolete timing fields if encountered; validate slot IDs and preserve first valid bindings. Export normal WAV-backed clips so durable material remains usable outside this performance mode. Durable checkpoint lists may omit degraded/coalesced session boundaries; do not label this unlimited session-by-session undo.

## Feasibility and verification gates

Before complete UI work, build a minimal hardware harness to demonstrate:

1. A real pinned 32 MiB pool alongside the minimum operating reserves, on OLED and seven-segment builds; if measured reserves require the authorized reduced profile, record that evidence and demonstrate capacity for eight ten-second stereo phrases instead. Failure to fit either supported profile fails the gate.
2. Eight independent RAM loops with continuous dry overdubs, short-loop multiwrap and immediate stop/start across repeated sessions.
3. Slow/stalled/removed SD during overdub without any storage dependency in live audio.
4. Worst-case eight-track simultaneous before-image copies, provisional double-tap buffers and snapshot cancellation without deadline misses.
5. Bounded metadata, memory and background queues after prolonged overdubbing, plus honest history degradation.
6. Measured pitch/FX CPU headroom and a declared supported load. A 128-frame window is about 2.90 ms at 44.1 kHz; use the actual render/DMA deadline, not this illustrative figure alone.

Instrument maximum callback execution, page-copy work, free-page lows, history evictions, snapshot cancellations, queue depths, archive write/sync latency and missed audio deadlines. Check output via captured audio, not LEDs alone. A supported performance envelope must be recorded from measurements; do not promise arbitrary additional song activity.

Host tests cover the state engine, exact frame content and saturation, phase, timing, multiwrap, source routing, no input after gate close, group actions, memory admission, history-chain validity, double-tap restoration without history space, snapshot consistency under concurrent edits/yielding writes, resource reclamation, save/load and fault handling. Use sanitizers for host audio/storage tests, build Debug firmware and the website, and preserve ordinary recorder lifecycle tests. Do not assume the five suites named in the original specification already exist.

## Builder discretion and review

Builder calls still to measure: page size, bounded history pool size, per-track scratch layout, reserve thresholds, transition-ramp length, scheduler batch sizes, archive retry policy and exact menu wording. These must honor the preferred 32 MiB profile or the explicitly authorized eight-by-ten-second fallback; they cannot introduce a smaller profile or insert storage waits into admitted overdubs. Record their measured values before implementation acceptance.

Design review must especially check that snapshots cannot force realtime allocation, double taps can discard provisional input under history pressure, the active pool fits beside all required reserves, and saved state never claims a mixed or incomplete generation. Use the authorized ten-second fallback only when measurements justify it. If both supported profiles fail feasibility, return with measurements and a proposed requirement change rather than reducing capacity further or limiting repeated overdubs.

## Self-contained behavioral baseline

The supplied September 8 algorithm specification is discussion input, not an external implementation dependency. The following rules, together with the sections above, are the baseline for this fresh implementation. Where the earlier text differs, this document takes precedence.

| State | Centre gesture | Result |
| --- | --- | --- |
| Empty | Press | Queue initial capture using Start Action |
| Recording | Press | Queue completion using Stop Action; enter Play/Dub per Record Action |
| Playing, loop | Press | Begin continuous overdub immediately, provisionally for double-tap handling |
| Playing, one-shot | Press | Queue retrigger from zero using Start Action |
| Overdubbing | Press | Close input immediately; LOOP continues playback, ONE_SHOT finishes the current pass and stops |
| Playing/overdubbing | Double press within 350 ms | Discard provisional additions; close input; queue backing stop using Stop Action |
| Stopped | Short release | Queue start from zero using Start Action |
| Stopped | Hold at least two seconds then release | Clear audio/history; retain identity, output and settings |
| Pending user action | Centre tap | Cancel that action, not an independent duration limit |
| Latched capture error | Press | Acknowledge only; subsequent press performs next action |

Storage-error indication is orthogonal to live state; it never suppresses live actions or rolls back audio. Mandatory provisional rollback only applies to additions belonging to the first tap of a possible double tap, not to an earlier confirmed continuous session. SHIFT-centre never triggers normal action or deletion; border press toggles selection. Long stopped holds fill all twelve border pads clockwise red and delete only on release. Hold timing and double-tap timing use a monotonic sample clock, independent of tempo.

Record Action defaults to REC > PLAY > DUB; alternative REC > DUB > PLAY enters automatic continuous overdub at the initial capture boundary. That automatic entry is not a provisional playing-tap gesture. Global Start Action and Stop Action each default to Synced, straight 1/16 bar, with Immediate available. Start All queues eligible stopped phrases at one deadline without restarting active loops; Stop All cancels queued starts. A repeated tap cancels a user pending action without deleting a fixed record-duration stop.

ONE_SHOT completion is suspended while its overdub capture gate is open, including automatic entry through REC > DUB > PLAY. The phrase wraps continuously during capture. A centre press ending that overdub closes input immediately, preserves the playback frame, and allows the current pass to finish before stopping at its native sample-frame boundary. It does not retrigger from zero or wait for a musical Stop Action deadline. A gate closed at frame zero leaves one complete pass to play. A stopped start or playing one-shot retrigger uses Start Action, begins at frame zero, and plays once. REC > PLAY > DUB initially enters ordinary one-shot playback; playing one-shot centre presses remain retriggers, as specified in the table. Explicit double-tap stop, Stop All, view exit, capture-error and transport-stop commands retain their documented policies.

Synced timing counts strictly future metronome pulses relative to metronome offset, including when the metronome is inaudible. For 1 bar count four future quarter pulses, 1/2 bar two quarter pulses, 1/4 bar one quarter pulse, 1/8 bar one half-quarter pulse, 1/16 bar one quarter-quarter pulse and 1/32 bar one eighth-quarter pulse. Triplets multiply pulse spacing by 2/3 with the same count. A gesture exactly on a pulse waits for the next. Preserve rational/fixed-point fractional precision, using Q16 positions where representable and explicit rounding rules; split audio windows at the first sample not earlier than the mathematical deadline. Tempo edits retime pending musical actions, never phrase frames.

XML modes: 0 Immediate, 1 Synced. Interval IDs: 0 bar, 1 half, 2 quarter, 3 eighth, 4 bar triplet, 5 half triplet, 6 quarter triplet, 7 eighth triplet, 8 sixteenth, 9 thirty-second, 10 sixteenth triplet, 11 thirty-second triplet. Menu order is six straight then six triplet choices. Invalid/missing modes become Synced; invalid/missing intervals become straight sixteenth. Preserve valid Immediate settings. New song settings are startActionMode, startActionInterval, stopActionMode and stopActionInterval under loopStation.

Per-track settings: physical Source LEFT (default), RIGHT, STEREO, BALANCED; Monitoring On by default; Record Duration Off/manual by default or whole bars from 1 to the smaller of 65535 and the firmware sequence-tick limit divided by current bar length, further constrained by available admitted memory; Playback LOOP by default or ONE_SHOT; Transpose -24.50 through +24.50 semitones at 0.01 resolution, default zero. Colour choices are Gold, Yellow, Lime, Turquoise, Cyan, Sky, Blue, Purple, Magenta, Pink, White. Defaults for slots 1-8: Yellow, Cyan, Blue, Purple, Magenta, White, Gold, Lime. Reserve exact red, orange and green for state indication. Hidden source-track/legacy timing/fade settings are not new exposed features.

Monitoring is audible for enabled capturing tracks; an enabled selected idle track monitors only when no capture consumes the same source. Recording remains dry regardless of monitoring. FX selection follows border selection. Transpose ratio is 2^(totalCents / 1200), converted to the firmware Q24 unity convention; semitone and cent components must normalize to the stated total range. Ordinary FX follow the combined transpose stage.

Buttons: Empty gray; Recording solid red; Overdubbing solid orange; Playing solid green; Stopped dim green; capture error fast red blink. Borders: empty dim song colour; recording red pulse; overdubbing orange pulse; playback uses a clockwise filling ring in the track colour, starting at physical top-left, with unfilled pads dim. The number of lit border pads follows each loop's actual sample-frame position and phrase length; a wrap resets the fill to the first pad. Stopped borders are dim track colour except the delete sweep; capture error borders are dim red. Background archive jobs do not replace musical state with COMMITTING. Show archive pending/error independently. Pending musical actions blink their destination indication. Physical top sidebar left is green Start All, right red Stop All; bright when eligible, dim when unavailable and blink while pending.

Capture input/resource failure closes only the affected gate, preserves existing live phrase and latches a slot error. Zero-frame initial takes do not become phrases. Source missing and RAM/resource failures must not become SD errors. Clear/history replacement/deletion are serialized with audio-boundary ownership changes; one slot's archive/transaction must not block other slots. Exit and transport during setup invalidate the setup generation so it cannot attach a late live capture. Reopening the view preserves all resident audio and background jobs.

Builder review performed by an independent firmware/audio reviewer. Its three findings (missing checked-in behavioral baseline, provisional audio entering snapshots, and evictable double-tap rollback) are addressed explicitly in this document. Hardware budgets and the safe bounded scratch-buffer reuse mechanism remain measured feasibility work, not established results.

The November 8 revision received a fresh independent builder check. Its remaining question concerned ONE_SHOT combined with automatic overdub. The user confirmed continuous wrapping during overdub and stopping at the end of the current pass after the centre press closes capture; that rule is now included above. No implementation or hardware acceptance is claimed by this design review.
