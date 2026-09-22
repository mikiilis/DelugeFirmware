> Historical request, superseded by [the Loop Station specification](../../../../LOOP_STATION_SPEC.md). Start and Stop now both default to Synced, straight 1/16 bar; available intervals extend through 1/32 bar, including triplets.

We need to simplify and refactor Start and Stop actions. First of all we need to remove Start and Stop transitions from loop track settings. Next in global loops station config, instead of record quintize and start stop timing and restart behaviour we need only 2 option, start and stop action (or another word). In each of these should be 2 options: Immediate or Synced.
Immediate - means the recording, playback actions starts or stops right when user click the button (loop track button or right panel button).
Synced - will have a miltiple options to postpone action to the next point in tempo. Option like: 1 bar (means the action wait for 4 clicks of metronome), 1/2 bar, 1/4 bar (1 click of metronome), 1/8 bar (half click of metronome). Also need option for triplet.

Overdub action always starts and stops immediate.

Example: user set start action option to 1/4 bar and stop action is 1 bar and pres record. The UI led indicates that record action is waiting, then it start recording then internal metronom click is happened. then user click to stop recording and continue recording for the next 1 bar (4 metronome click). On 4th click the recording stops and stage change to next (playback/overdub) immediatelly. Then user douple tap on button to stop loop track, it continue playing for 4 metronome tacts and stop. During this time the led UI display that stop action is pending.

Default values:
start - synced 1/4 bar (next click of metronome)
stop - immediate

When tempo is changed after loop recorded - do nothing with already recorded loops.
Make a plan for implementation and refactoring, also update the spec and documentation files.
