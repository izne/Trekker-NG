#include "eq.h"

#include <cmath>

namespace tng {

namespace {

// Crossover frequencies (user decision 2026-10-05): the split where a DJ
// mixer's low/high sections typically sit - low band below 250 Hz, high
// band above 4 kHz, everything else is the mid band.
constexpr float kFcLowHz = 250.0f;
constexpr float kFcHighHz = 4000.0f;

constexpr float kTauSec = 0.012f; // knob smoothing (~12 ms)

} // namespace

float Eq3::curve(float knob) noexcept {
    if (knob < 0.0f) knob = 0.0f;
    if (knob > 1.0f) knob = 1.0f;
    if (knob >= 0.5f) {
        // Boost side: dB-linear, 0 dB at center -> +6 dB at full CW.
        const float db = (knob - 0.5f) * 12.0f;
        return std::pow(10.0f, db / 20.0f);
    }
    // Cut side: linear-amplitude cubic - gentle right of center, exact
    // silence at full CCW (t = knob * 2 maps 0..0.5 onto 0..1).
    const float t = knob * 2.0f;
    return t * t * t;
}

Eq3::Eq3() noexcept { updateCrossover(); }

void Eq3::setSampleRate(uint32_t fs) noexcept {
    fs_ = fs > 0 ? fs : 44100;
    updateCrossover();
}

void Eq3::setTarget(int band, float knob) noexcept {
    if (band < 0 || band >= kBands) return;
    if (knob < 0.0f) knob = 0.0f;
    if (knob > 1.0f) knob = 1.0f;
    target_[band] = knob;
}

void Eq3::beginBlock(uint32_t frameCount) noexcept {
    // One-pole step covering the whole block: after ~12 ms of audio the
    // smoothed knob is within 1% of the target (settles in a few blocks).
    const float alpha =
        frameCount > 0
            ? 1.0f - std::exp(-static_cast<float>(frameCount) /
                              (kTauSec * static_cast<float>(fs_)))
            : 0.0f;
    for (int b = 0; b < kBands; ++b) {
        smooth_[b] += (target_[b] - smooth_[b]) * alpha;
        gain_[b] = curve(smooth_[b]);
    }
}

void Eq3::updateCrossover() noexcept {
    const float fs = static_cast<float>(fs_);
    const float fcs[2] = {kFcLowHz, kFcHighHz};
    Biquad* lp[2] = {lpLow_, lpHigh_};
    Biquad* hp[2] = {hpLow_, hpHigh_};
    for (int c = 0; c < 2; ++c) {
        const float w0 = 6.28318530718f * fcs[c] / fs;
        const float cw = std::cos(w0);
        const float alpha = std::sin(w0) * 0.70710678118f; // Butterworth Q
        const float a0 = 1.0f + alpha;
        const float a1 = -2.0f * cw;
        const float a2 = 1.0f - alpha;
        const float lpB0 = (1.0f - cw) * 0.5f;
        const float hpB0 = (1.0f + cw) * 0.5f;
        // LR4 = two identical Butterworth 2nd-order sections in series
        // (separate states, same coefficients).
        for (int stage = 0; stage < 2; ++stage) {
            lp[c][stage].set(lpB0, 2.0f * lpB0, lpB0, a0, a1, a2);
            hp[c][stage].set(hpB0, -2.0f * hpB0, hpB0, a0, a1, a2);
        }
    }
}

void Eq3::Biquad::set(float nb0, float nb1, float nb2, float na0, float na1,
                      float na2) noexcept {
    const float inv = 1.0f / na0;
    b0 = nb0 * inv;
    b1 = nb1 * inv;
    b2 = nb2 * inv;
    a1 = na1 * inv;
    a2 = na2 * inv;
}

float Eq3::Biquad::run(int channel, float x) noexcept {
    State& s = st[channel == 0 ? 0 : 1];
    const float y = b0 * x + b1 * s.x1 + b2 * s.x2 - a1 * s.y1 - a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = x;
    s.y2 = s.y1;
    // Flush dead states so a quiet input cannot leave the recursion
    // crawling through denormals (SPEC §4.7: no priority spikes).
    s.y1 = (std::fabs(y) < 1.0e-15f) ? 0.0f : y;
    return y;
}

float Eq3::process(int channel, float x) noexcept {
    float low = lpLow_[0].run(channel, x);
    low = lpLow_[1].run(channel, low);

    float rest = hpLow_[0].run(channel, x);
    rest = hpLow_[1].run(channel, rest);

    float mid = lpHigh_[0].run(channel, rest);
    mid = lpHigh_[1].run(channel, mid);

    float high = hpHigh_[0].run(channel, rest);
    high = hpHigh_[1].run(channel, high);

    return low * gain_[0] + mid * gain_[1] + high * gain_[2];
}

} // namespace tng
