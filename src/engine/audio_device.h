#pragma once

#include <cstdint>
#include <string>

namespace sde {

class Deck;

// Thin wrapper around a miniaudio playback device (SPEC §4.1 output stage).
// The callback only calls Deck::render() plus a master gain/clamp - no
// allocation, locks, IO, logging or exceptions (SPEC §4.7).
class AudioDevice {
public:
    // Public only so the C callback can name the type; it is still an
    // incomplete pimpl outside audio_device.cpp.
    struct Impl;

    AudioDevice();
    ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // deck must outlive the device. sampleRate comes from the loaded track;
    // periodFrames is the callback size (default 256 frames, SPEC §4.7).
    bool init(Deck* deck, uint32_t sampleRate, uint32_t periodFrames, std::string* error);
    bool start(std::string* error);
    void stop();
    void shutdown();

    void setMasterGain(float gain) noexcept;
    float masterGain() const noexcept;

private:
    Impl* impl_;
};

} // namespace sde
