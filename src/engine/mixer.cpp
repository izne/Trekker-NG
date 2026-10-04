#include "mixer.h"

#include <cmath>

namespace tng {

namespace {

float clampUnit(float v) noexcept {
    if (v > 1.0f) return 1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

} // namespace

void Mixer::process(const float* deckA, const float* deckB, float* out,
                    uint32_t frameCount) noexcept {
    const float lineA = lineA_.load(std::memory_order_relaxed);
    const float lineB = lineB_.load(std::memory_order_relaxed);
    const float master = masterGain_.load(std::memory_order_relaxed);

    // Target gains for this block: equal-power crossfade * line gain.
    float targetA;
    float targetB;
    if (deckB == nullptr) {
        targetA = lineA; // single-deck mode: crossfader and line B bypassed
        targetB = 0.0f;
    } else {
        const float xf = crossfader_.load(std::memory_order_relaxed);
        const float angle = (xf + 1.0f) * 0.78539816339f; // (xf + 1) * pi/4
        targetA = std::cos(angle) * lineA;
        targetB = std::sin(angle) * lineB;
    }

    // Linear ramp across the block: the first sample steps away from the
    // previous block's gain, the last sample lands exactly on the target.
    const float stepA = frameCount > 0 ? (targetA - rampA_) / static_cast<float>(frameCount)
                                       : 0.0f;
    const float stepB = frameCount > 0 ? (targetB - rampB_) / static_cast<float>(frameCount)
                                       : 0.0f;

    if (deckB == nullptr) {
        for (uint32_t i = 0; i < frameCount; ++i) {
            rampA_ += stepA;
            const float gain = rampA_ * master;
            const uint32_t k = i * 2;
            out[k] = clampUnit(deckA[k] * gain);
            out[k + 1] = clampUnit(deckA[k + 1] * gain);
        }
    } else {
        for (uint32_t i = 0; i < frameCount; ++i) {
            rampA_ += stepA;
            rampB_ += stepB;
            const float gainA = rampA_ * master;
            const float gainB = rampB_ * master;
            const uint32_t k = i * 2;
            out[k] = clampUnit(deckA[k] * gainA + deckB[k] * gainB);
            out[k + 1] = clampUnit(deckA[k + 1] * gainA + deckB[k + 1] * gainB);
        }
    }

    // Land exactly on the targets - kills accumulated float drift so the
    // next block's step starts from a clean value.
    rampA_ = targetA;
    rampB_ = targetB;
}

void Mixer::setLineGain(int deck, float gain) noexcept {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    if (deck == 0) {
        lineA_.store(gain, std::memory_order_relaxed);
    } else {
        lineB_.store(gain, std::memory_order_relaxed);
    }
}

float Mixer::lineGain(int deck) const noexcept {
    return (deck == 0 ? lineA_ : lineB_).load(std::memory_order_relaxed);
}

void Mixer::setCrossfader(float position) noexcept {
    if (position < -1.0f) position = -1.0f;
    if (position > 1.0f) position = 1.0f;
    crossfader_.store(position, std::memory_order_relaxed);
}

float Mixer::crossfader() const noexcept {
    return crossfader_.load(std::memory_order_relaxed);
}

void Mixer::setMasterGain(float gain) noexcept {
    masterGain_.store(gain, std::memory_order_relaxed);
}

float Mixer::masterGain() const noexcept {
    return masterGain_.load(std::memory_order_relaxed);
}

} // namespace tng
