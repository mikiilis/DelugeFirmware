# Loop Station

Loop Station is an eight-track, RAM-resident audio looper for live performance. Each track can record a phrase, play it in a loop, and overdub new audio. Playback is independent of SD-card writes, so recording and overdubbing do not wait for storage activity.

> Loop Station is a community firmware feature and is disabled by default. The [interactive demo](../../website/public/demos/loop-station.html) demonstrates the pad layout; it does not connect to a Deluge or capture audio.

## Try the interactive view

Open `website/public/demos/loop-station.html` in a browser to explore the eight-track illustration. This file works directly without a website server. The same view is embedded in the website's Loop Station guide, with a link to open it at full size.

The demo starts with playing, recording, overdubbing, stopped and empty tracks. Playing tracks have independent phrase lengths and clockwise filling rings. Try the center pads, double-press to stop, hold a stopped center for two seconds and release to clear, or use the sidebar Start All and Stop All buttons. Pause Animation freezes the ring motion, and Reset Demo restores the starting examples. Timing is illustrative and the demo is silent.

## What it can do

- Record and play up to eight audio phrases at once.
- Overdub a playing phrase and keep each track at its own length.
- Select tracks and watch their clockwise progress rings advance during playback.
- Start or stop all tracks together, undo supported edits, and clear stopped tracks.
- Set input source, monitoring, duration, playback mode, transpose and track color for each track.
- Open a track's normal Audio Clip view to use its standard output and effects.
- Save loop audio with the normal Song save workflow and restore committed checkpoints when loading the Song.

Loop audio is held in a shared memory pool. The configured profile reserves 32 MiB for active stereo audio. At 44.1 kHz and eight bytes per frame, this is about 95 seconds combined before page overhead, or about 11.9 seconds per track if shared evenly. This is a planning estimate, not a hardware-tested duration guarantee; actual limits depend on available pages and memory.

## Turn it on and enter

1. Open **Settings → Community Features**.
2. Set **Enable Loop Station** to **On**.
3. Return to the idle Song/Session View root.
4. Press **CROSS-SCREEN** once without holding Shift.

Loop Station reserves its configured audio memory when opening. If the reservation fails, the current view remains open and the device reports the failure. Entering Loop Station does not start transport or recording. Press **CROSS-SCREEN** or **SONG** to exit; an active capture may delay exit until it is safe.

## Use the eight track pads

The Deluge's 16×8 pad grid is divided into eight 4×4 tiles: tracks 1–4 across the top and tracks 5–8 across the bottom. In each tile, the center 2×2 pads control the track and the surrounding 12 pads form its selection/progress ring.

1. Press the center of an empty track to record its first take.
2. Press the center again to finish the take. **Record Action** decides whether the phrase starts playing or moves directly into overdub.
3. Press the center of a playing track to begin overdubbing; press it again to close the capture gate. A quick double press on a playing track stops it.
4. Press a track's outer ring to select it. While it plays, the ring fills clockwise from the upper-left pad and resets each time the phrase loops.
5. Press **PLAY** to Start All and **RECORD** to record on the selected track. The physical top row of the sidebar provides Start All (left) and Stop All (right); the other sidebar rows are inactive.
6. Stop a track, then hold its center for two seconds and release to clear it. A red sweep shows hold progress.
7. Press **BACK** on a track with available history to request undo. The display indicates when undo is unavailable or still pending.

Track colors distinguish tracks. New tracks default to **Yellow, Cyan, Blue, Purple, Magenta, Pink, Gold and Lime** for tracks 1–8 respectively. Center pads and rings show empty, recording, overdubbing, playing and stopped states. These temporary state colors take priority over the chosen identity color. A capture error appears on the affected track; press its center to acknowledge it.

## Change settings

- Press **SELECT** to open global Loop Station settings.
- Hold **SHIFT** and press a track's center to open that track's settings.
- Turn the Select encoder to move through rows, press Select to edit, and press **BACK** to return.

Global settings include Record Action, Start Mode/Interval and Stop Mode/Interval. Interval settings are shown when the related mode is Synced. Per-track settings include input source, monitoring, record duration, Loop or One Shot playback, transpose, color and Copy To Track. Transpose range is ±24.50 semitones in one-cent steps. Copy To Track copies settings only; it leaves the destination audio, name, output, effects and history intact.

To change a ring's identity color: **SHIFT + track centre → Colour → SELECT to edit → turn SELECT → SELECT to finish → BACK**. The ring updates live and the menu shows the chosen color. White remains available, and multiple tracks may use the same color. Return to Song/Session View and use normal Song **SAVE** to store changes; normal Song load restores them. Editing does not write to the SD card on each turn. Existing saved/custom colors are preserved, including White and copied or duplicate choices; the new defaults do not recolor saved Songs.

Press **CLIP VIEW** to open the selected track's ordinary Audio Clip view and menus. Loop tracks use normal Song outputs and effects.

## Save and undo

Use the normal Song **SAVE** workflow to save a durable loop checkpoint. A save can remain pending while a consistent audio snapshot is written. Audio recorded after that snapshot is not included until a later successful save. If the card reports an error, the current RAM audio remains available but is not durable.

Undo history is bounded. Under memory or storage pressure, older history may be discarded while current capture continues. Loading a Song restores its committed loop checkpoints; incomplete or failed saves do not replace the last committed manifest. Saved audio uses cumulative 24-bit, 44.1 kHz WAV checkpoints, so reload is not a bit-identical 32-bit RAM round trip.

## Validation status

Host tests cover timing, recording actions, input/monitor routing, different phrase lengths, gestures, history pressure, archive faults and restart recovery. They do not measure hardware memory, full audio callback deadlines, physical input latency or recorded output quality. Physical hardware, OLED/seven-segment inspection, hardware memory/DSP measurements and audio-capture acceptance remain **NOT_RUN**.
