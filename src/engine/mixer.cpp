#include "mixer.h"

namespace sde {

void Mixer::process(float* out, uint32_t frameCount) noexcept {
    const float gain = masterGain_.load(std::memory_order_relaxed);
    const uint32_t count = frameCount * 2; // stereo
    for (uint32_t i = 0; i < count; ++i) {
        float v = out[i] * gain;
        // Hard clamp until the soft limiter arrives (M5, SPEC §4.6): keeps
        // summed full-scale stems from wrapping at the DAC.
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        out[i] = v;
    }
}

void Mixer::setMasterGain(float gain) noexcept {
    masterGain_.store(gain, std::memory_order_relaxed);
}

float Mixer::masterGain() const noexcept {
    return masterGain_.load(std::memory_order_relaxed);
}

} // namespace sde
