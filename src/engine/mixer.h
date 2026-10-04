#pragma once

#include <atomic>
#include <cstdint>

namespace tng {

// Master section of the mixer (SPEC §4.1/§4.6): master gain + safety clamp.
// The crossfader, line faders and EQ arrive with the two-deck UI (M4/M5).
//
// process() runs inside the audio callback: no allocation, no locks, no IO,
// no exceptions (SPEC §4.7) - just an atomic load, a multiply and a clamp.
class Mixer {
public:
    void process(float* interleavedStereo, uint32_t frameCount) noexcept;

    void setMasterGain(float gain) noexcept;
    float masterGain() const noexcept;

private:
    std::atomic<float> masterGain_{0.7f}; // M1/M2 default until the master section
};

} // namespace tng
