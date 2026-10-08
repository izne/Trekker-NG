#include "mixer.h"

#include <algorithm>
#include <cmath>

namespace tng {

namespace {

// M5b soft-clip limiter (SPEC §4.6): below the knee the signal passes
// bit-exact; above it a tanh knee maps (knee, inf) -> (knee, 1) and never
// exceeds +-1. Zero state, pure math - realtime safe.
constexpr float kClipKnee = 0.9f;
constexpr float kClipScale = 1.0f - kClipKnee; // headroom fed into tanh

float softClip(float v, bool& hit) noexcept {
    const float a = std::fabs(v);
    if (a <= kClipKnee) return v;
    hit = true;
    const float y =
        kClipKnee + kClipScale * std::tanh((a - kClipKnee) / kClipScale);
    return v < 0.0f ? -y : y;
}

} // namespace

Mixer::Mixer() noexcept {
    for (int d = 0; d < 2; ++d) {
        for (int b = 0; b < Eq3::kBands; ++b) {
            eqKnob_[d][b].store(0.5f, std::memory_order_relaxed); // flat
        }
    }
}

void Mixer::process(const float* deckA, const float* deckB, float* out,
                    uint32_t frameCount) noexcept {
    const float lineA = lineA_.load(std::memory_order_relaxed);
    const float lineB = lineB_.load(std::memory_order_relaxed);
    const float master = masterGain_.load(std::memory_order_relaxed);

    // M5a: refresh the EQ for this block (smoothed knobs -> coefficients).
    for (int b = 0; b < Eq3::kBands; ++b) {
        eqA_.setTarget(b, eqKnob_[0][b].load(std::memory_order_relaxed));
        eqB_.setTarget(b, eqKnob_[1][b].load(std::memory_order_relaxed));
    }
    eqA_.beginBlock(frameCount);
    eqB_.beginBlock(frameCount);

    // Target gains for this block: crossfader curve * line gain.
    float targetA;
    float targetB;
    if (deckB == nullptr) {
        targetA = lineA; // single-deck mode: crossfader and line B bypassed
        targetB = 0.0f;
    } else {
        const float xf = crossfader_.load(std::memory_order_relaxed);
        const int curve = xfCurve_.load(std::memory_order_relaxed);
        const float t = (xf + 1.0f) * 0.5f; // 0 = full A, 1 = full B
        float ga;
        float gb;
        if (curve == kXfLinear) {
            ga = 1.0f - t;
            gb = t;
        } else if (curve == kXfSharpCut) {
            // Full volume until the last 5% of the travel toward the other
            // deck (slope k = 20), then a linear fall to 0 at the end.
            ga = std::min(1.0f, (1.0f - t) * 20.0f);
            gb = std::min(1.0f, t * 20.0f);
        } else {
            // Constant power (M4 default): equal-power cos/sin pair.
            const float angle = (xf + 1.0f) * 0.78539816339f; // (xf + 1) * pi/4
            ga = std::cos(angle);
            gb = std::sin(angle);
        }
        targetA = ga * lineA;
        targetB = gb * lineB;
    }

    // Linear ramp across the block: the first sample steps away from the
    // previous block's gain, the last sample lands exactly on the target.
    const float stepA = frameCount > 0 ? (targetA - rampA_) / static_cast<float>(frameCount)
                                       : 0.0f;
    const float stepB = frameCount > 0 ? (targetB - rampB_) / static_cast<float>(frameCount)
                                       : 0.0f;

    bool clipHit = false;
    if (deckB == nullptr) {
        // Single-deck path (console, legacy tests): no EQ stage, no
        // crossfader - the live UI always registers both decks and goes
        // through the two-deck path below. The master soft clip applies
        // here too (M5b: it is the master stage).
        for (uint32_t i = 0; i < frameCount; ++i) {
            rampA_ += stepA;
            const float gain = rampA_ * master;
            const uint32_t k = i * 2;
            out[k] = softClip(deckA[k] * gain, clipHit);
            out[k + 1] = softClip(deckA[k + 1] * gain, clipHit);
        }
    } else {
        for (uint32_t i = 0; i < frameCount; ++i) {
            rampA_ += stepA;
            rampB_ += stepB;
            const float gainA = rampA_ * master;
            const float gainB = rampB_ * master;
            const uint32_t k = i * 2;
            // deckA/deckB may alias out: both decks are read through the
            // EQ before either output sample is written.
            const float aL = eqA_.process(0, deckA[k]);
            const float aR = eqA_.process(1, deckA[k + 1]);
            const float bL = eqB_.process(0, deckB[k]);
            const float bR = eqB_.process(1, deckB[k + 1]);
            out[k] = softClip(aL * gainA + bL * gainB, clipHit);
            out[k + 1] = softClip(aR * gainA + bR * gainB, clipHit);
        }
    }
    if (clipHit) clipFlag_.store(true, std::memory_order_relaxed);

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

void Mixer::setXfCurve(int curve) noexcept {
    if (curve < kXfLinear) curve = kXfLinear;
    if (curve > kXfSharpCut) curve = kXfSharpCut;
    xfCurve_.store(curve, std::memory_order_relaxed);
}

int Mixer::xfCurve() const noexcept {
    return xfCurve_.load(std::memory_order_relaxed);
}

bool Mixer::takeClipFlag() noexcept {
    return clipFlag_.exchange(false, std::memory_order_relaxed);
}

void Mixer::setMasterGain(float gain) noexcept {
    masterGain_.store(gain, std::memory_order_relaxed);
}

float Mixer::masterGain() const noexcept {
    return masterGain_.load(std::memory_order_relaxed);
}

void Mixer::setEqKnob(int deck, int band, float knob) noexcept {
    if (deck < 0 || deck > 1 || band < 0 || band >= Eq3::kBands) return;
    if (knob < 0.0f) knob = 0.0f;
    if (knob > 1.0f) knob = 1.0f;
    eqKnob_[deck][band].store(knob, std::memory_order_relaxed);
}

float Mixer::eqKnob(int deck, int band) const noexcept {
    if (deck < 0 || deck > 1 || band < 0 || band >= Eq3::kBands) return 0.5f;
    return eqKnob_[deck][band].load(std::memory_order_relaxed);
}

void Mixer::setSampleRate(uint32_t fs) noexcept {
    eqA_.setSampleRate(fs);
    eqB_.setSampleRate(fs);
}

} // namespace tng
