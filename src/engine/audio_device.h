#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tng {

class Deck;
class Mixer;
class Recorder;

// Thin wrapper around a miniaudio playback device (SPEC §4.1 output stage).
// The callback only calls Deck::render() + Mixer::process() (+ the M7
// Recorder tap: one ring copy) - no allocation, locks, IO, logging or
// exceptions (SPEC §4.7).
class AudioDevice {
public:
    // Public only so the C callback can name the type; it is still an
    // incomplete pimpl outside audio_device.cpp.
    struct Impl;

    AudioDevice();
    ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // M5c: playback device names for the settings screen. names[0] is
    // always "" (the system default, shown as "(default)"); the rest are
    // the enumerated devices by name. Setup-time only - never called from
    // the audio callback. Returns false when enumeration failed (the list
    // keeps just the default entry).
    static bool listPlaybackDevices(std::vector<std::string>* names);

    // deckA/deckB and mixer must outlive the device. deckB may be null
    // (single-deck console / M3 UI: mixer bypasses the crossfader).
    // sampleRate comes from the loaded track; periodFrames is the callback
    // size (256, SPEC §4.7). deviceName null or empty = system default
    // (M5c); otherwise it must be one of the listPlaybackDevices() names.
    // recorder (M7) may be null; otherwise it must outlive the device too -
    // the callback taps the master output into its ring every block.
    bool init(Deck* deckA, Deck* deckB, Mixer* mixer, uint32_t sampleRate, uint32_t periodFrames,
              const char* deviceName, std::string* error, Recorder* recorder = nullptr);
    bool start(std::string* error);
    void stop();
    void shutdown();

private:
    Impl* impl_;
};

} // namespace tng

