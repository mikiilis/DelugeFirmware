# Loop Station Real-Time Transpose Implementation Plan

## Overview
Add a **Transpose** parameter to the Loop Station track menu that applies real-time pitch shifting to the monitored input signal during recording/overdubbing/playback when monitoring is enabled.

The transpose should affect both:
1. **Live monitoring** - what you hear in real-time while recording
2. **Recorded audio** - the captured WAV file (already works via `sampleHolder.transpose`)

---

## Architecture Changes

### 1. Core Data Model (`src/deluge/loop_station/loop_station.h`)

**Add to `TrackParameter` enum:**
```cpp
enum class TrackParameter : uint8_t {
    // ... existing ...
    FADE_TIME_BARS,
    TRANSPOSE  // NEW: semitone transpose (-24 to +24)
};
```

**Add to `TrackSettings` struct:**
```cpp
struct TrackSettings {
    // ... existing ...
    uint16_t fadeTimeBars = 1;
    int16_t transpose = 0;           // NEW: semitones (-24 to +24)
    int8_t transposeCents = 0;       // NEW: fine tune (-50 to +50)
};
```

**Update read/write functions in `loop_station.cpp`:**
```cpp
// In readTrackParameter():
case TrackParameter::TRANSPOSE:
    return settings.transpose * 100 + settings.transposeCents;  // Return in cents

// In writeTrackParameter():
case TrackParameter::TRANSPOSE:
    {
        int32_t valueInCents = std::clamp<int32_t>(value, -2400, 2400);
        settings.transpose = static_cast<int16_t>(valueInCents / 100);
        settings.transposeCents = static_cast<int8_t>(valueInCents % 100);
    }
    break;
```

**Update `SongModel::sanitize()` to clamp transpose values.**

---

### 2. Menu Integration (`src/deluge/gui/menu_item/loop_station/settings.cpp`)

**Add new menu item class:**
```cpp
class TrackTranspose final : public IntegerWithCents {
public:
    using IntegerWithCents::IntegerWithCents;

    void readCurrentValue() override {
        auto const& settings = model().tracks[static_cast<size_t>(activeSlot)].settings;
        setValue(settings.transpose * 100 + settings.transposeCents);
    }

    void writeCurrentValue() override {
        int32_t valueInCents = std::clamp<int32_t>(getValue(), -2400, 2400);
        auto& settings = model().tracks[static_cast<size_t>(activeSlot)].settings;
        settings.transpose = static_cast<int16_t>(valueInCents / 100);
        settings.transposeCents = static_cast<int8_t>(valueInCents % 100);
        writeTrackParameter(model(), activeSlot, TrackParameter::TRANSPOSE, valueInCents);
        
        // Apply immediately to live monitoring if active
        if (engine_) {
            loopStationView.refreshMonitoring();
        }
    }

    [[nodiscard]] int32_t getMinValue() const override { return -2400; }
    [[nodiscard]] int32_t getMaxValue() const override { return 2400; }
    [[nodiscard]] int32_t getNumDecimalPlaces() const override { return 2; }
    const char* getUnit() override { return "st"; }
};
```

**Register in `trackSettingsMenu`:**
```cpp
PLACE_SDRAM_BSS TrackTranspose transpose{l10n::String::STRING_FOR_LOOP_STATION_TRANSPOSE, TrackParameter::TRANSPOSE};

// Add to menu items list:
{&source, &sourceTrack, &monitoring, &duration, &progressColour, &playback,
 &loopSync, &startMode, &stopMode, &fadeTime, &transpose,  // NEW
 &audioClipActionsMenu, ...}
```

**Add localization string** (`src/deluge/gui/l10n/`):
- `STRING_FOR_LOOP_STATION_TRANSPOSE` = "Transpose"

---

### 3. Real-Time Pitch Shifting Implementation

**New file: `src/deluge/processing/live/input_pitch_shifter.h/.cpp`**
```cpp
// Simple real-time pitch shifter for monitoring path
// Uses linear interpolation for efficiency (good enough for monitoring)
class InputPitchShifter {
public:
    InputPitchShifter();
    void setPitchRatio(float ratio);  // 0.5 = -12st, 1.0 = 0st, 2.0 = +12st
    void process(std::span<StereoSample> inputOutput, int32_t numSamples);
    
private:
    float pitchRatio_ = 1.0f;
    float phase_ = 0.0f;
    StereoSample prevSample_{};
    StereoSample currSample_{};
    bool firstSample_ = true;
};
```

**Integration in `AudioOutput::renderGlobalEffectableForClip()`:**

Modify the monitoring path (around line 346) to apply pitch shifting:
```cpp
// add in the monitored audio if in sampler or looper mode
if (modeAllowsMonitoring() && modelStack->song->isOutputActiveInArrangement(this)
    && inputChannel != AudioInputChannel::SPECIFIC_OUTPUT) {
    rendered = true;
    
    // NEW: Get transpose setting for Loop Station slots
    float pitchRatio = 1.0f;
    if (loopStationMonitoring) {
        // Find the slot this output belongs to and get its transpose
        // pitchRatio = calculatePitchRatio(slotTranspose);
    }
    
    if (pitchRatio != 1.0f) {
        // Apply pitch shifting to monitored signal
        applyPitchShiftToMonitor(output, inputChannel, amplitudeAtStart, amplitudeAtEnd, 2, pitchRatio);
    } else {
        addPhysicalInput(output, inputChannel, amplitudeAtStart, amplitudeAtEnd, 2);
    }
}
```

**Also apply to overdub capture path (line 330):**
```cpp
if (recorder->captureOutputInputActive && isPhysicalInput(inputChannel)) {
    float pitchRatio = getTransposePitchRatioForSlot(slot);
    if (pitchRatio != 1.0f) {
        applyPitchShiftToCapture(capture, inputChannel, amplitudeAtStart, amplitudeAtEnd, 3, true, pitchRatio);
    } else {
        addPhysicalInput(capture, inputChannel, amplitudeAtStart, amplitudeAtEnd, 3, true);
    }
}
```

---

### 4. Pitch Ratio Calculation

```cpp
// Helper function in audio_output.cpp or loop_station.h
constexpr float semitonesToRatio(int16_t semitones, int8_t cents) {
    float totalSemitones = static_cast<float>(semitones) + static_cast<float>(cents) / 100.0f;
    return std::pow(2.0f, totalSemitones / 12.0f);
}
```

---

### 5. Apply Transpose to Recorded Audio

The recorded audio should also have the transpose baked in. This is already handled by the existing `sampleHolder.transpose` mechanism, but we need to sync the Loop Station track transpose to the clip's sample holder.

**In `LoopStationView::finishCapture()` (around line 346):**
```cpp
if (accepted) {
    clip->loopStationSampleZeroTicks = overdub ? replacementSampleZero : 0;
    
    // NEW: Apply Loop Station track transpose to the recorded clip
    auto const& settings = currentSong->loopStation.tracks[static_cast<size_t>(slot)].settings;
    if (settings.transpose != 0 || settings.transposeCents != 0) {
        clip->sampleHolder.transpose = settings.transpose;
        clip->sampleHolder.cents = settings.transposeCents;
        clip->sampleHolder.recalculateNeutralPhaseIncrement();
    }
    
    if (!overdub) { ... }
}
```

---

## Detailed Implementation Steps

| Step | Component | File | Description |
|------|-----------|------|-------------|
| 1 | Data Model | `loop_station.h` | Add `TRANSPOSE` to `TrackParameter` enum |
| 2 | Data Model | `loop_station.h` | Add `transpose` and `transposeCents` to `TrackSettings` |
| 3 | Data Model | `loop_station.cpp` | Implement read/write for `TrackParameter::TRANSPOSE` |
| 4 | Data Model | `loop_station.cpp` | Update `SongModel::sanitize()` to clamp transpose values |
| 5 | Menu | `settings.cpp` | Add `TrackTranspose` menu item class |
| 6 | Menu | `settings.cpp` | Register transpose in `trackSettingsMenu` |
| 7 | Localization | `l10n/` files | Add `STRING_FOR_LOOP_STATION_TRANSPOSE` |
| 8 | DSP | `processing/live/input_pitch_shifter.h/.cpp` | Create lightweight pitch shifter for monitoring |
| 9 | Audio Path | `audio_output.cpp` | Apply pitch shift in monitoring path (`addPhysicalInput` call) |
| 10 | Audio Path | `audio_output.cpp` | Apply pitch shift in overdub capture path |
| 11 | Recording | `loop_station_view.cpp` | Sync transpose to recorded clip's `sampleHolder` |
| 12 | Testing | `loop_station_tests.cpp` | Add unit tests for transpose parameter |
| 13 | Integration | Build & test | Verify real-time monitoring pitch shift works |

---

## Key Design Decisions

1. **Pitch Shifting Algorithm**: Use a simple linear-interpolation based pitch shifter for the monitoring path (low latency, good enough for monitoring). The `LivePitchShifter` is too heavyweight for this use case.

2. **Transpose Range**: ±24 semitones (±2 octaves) with cent precision (-50 to +50 cents), matching the existing audio clip transpose range.

3. **Application Points**:
   - **Monitoring path** (audible output): Real-time pitch shift applied in `AudioOutput::renderGlobalEffectableForClip()`
   - **Capture path** (overdub recording): Same pitch shift applied so recorded audio matches what's heard
   - **Recorded clip**: Transpose baked into `sampleHolder` for playback consistency

4. **Performance**: The pitch shifter runs per-track only when monitoring is active and transpose ≠ 0. With 8 tracks max, CPU impact is minimal.

5. **State Sync**: When transpose parameter changes, call `refreshMonitoring()` to update the live monitoring immediately.

---

## Clarifying Questions

1. **Should transpose persist per-slot?** Yes, it's part of `TrackSettings` which is per-slot and saved with the song.

2. **Should transpose affect playback of existing loops?** The plan applies transpose to monitoring and recording. For playback of existing loops, the existing `sampleHolder.transpose` on the clip handles it. Should the Loop Station track transpose *override* or *add to* the clip's transpose during playback?

3. **Mono vs Stereo monitoring**: The pitch shifter should handle both mono and stereo input channels correctly.

4. **Latency tolerance**: What's the acceptable latency for the monitoring path? A simple linear interpolation pitch shifter adds ~1-2 samples of latency.

---

## Files to Modify

1. `src/deluge/loop_station/loop_station.h` - Add parameter enum and settings fields
2. `src/deluge/loop_station/loop_station.cpp` - Implement read/write/sanitize
3. `src/deluge/gui/menu_item/loop_station/settings.cpp` - Add menu item
4. `src/deluge/gui/l10n/` - Add localization string
5. `src/deluge/processing/live/input_pitch_shifter.h/.cpp` - **NEW** lightweight pitch shifter
6. `src/deluge/processing/audio_output.cpp` - Apply pitch shift in monitoring/capture paths
7. `src/deluge/gui/views/loop_station_view.cpp` - Sync transpose to recorded clip
8. `tests/unit/loop_station_tests.cpp` - Add tests

---

## Verification Plan

1. **Unit Tests**: Test transpose parameter read/write/clamp in `loop_station_tests.cpp`
2. **Manual Test**: 
   - Enable Loop Station
   - Set transpose to +12 (octave up)
   - Enable monitoring on a track
   - Record audio - verify monitoring hears pitched audio in real-time
   - Playback recorded loop - verify it's pitched up
3. **Edge Cases**: 
   - Transpose during overdub (should affect new layer only)
   - Transpose change during playback (should update monitoring immediately)
   - Negative transpose (pitch down)
   - Cent values (fine tune)