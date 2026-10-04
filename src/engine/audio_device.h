#pragma once

#include <cstdint>
#include <string>

namespace tng {

class Deck;
class Mixer;

// Thin wrapper around a miniaudio playback device (SPEC §4.1 output stage).
// The callback only calls Deck::render() + Mixer::process() - no allocation,
// locks, IO, logging or exceptions (SPEC §4.7).
class AudioDevice {
public:
    // Public only so the C callback can name the type; it is still an
    // incomplete pimpl outside audio_device.cpp.
    struct Impl;

    AudioDevice();
    ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // deck and mixer must outlive the device. sampleRate comes from the
    // loaded track; periodFrames is the callback size (256, SPEC §4.7).
    bool init(Deck* deck, Mixer* mixer, uint32_t sampleRate, uint32_t periodFrames,
              std::string* error);
    bool start(std::string* error);
    void stop();
    void shutdown();

private:
    Impl* impl_;
};

} // namespace tng
