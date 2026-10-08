#pragma once

#include <cstdint>

namespace tng {

// 3-band deck EQ (SPEC §4.6, M5a): LR4 crossover band-split - the "EQ3"
// topology Traktor uses. Per deck, two Butterworth 2nd-order sections per
// crossover split the signal into bands, each band gets its gain, the
// bands sum back:
//
//   low  = LP4(fcLow)                 * gainLow
//   mid  = HP4(fcLow) -> LP4(fcHigh)  * gainMid
//   high = HP4(fcLow) -> HP4(fcHigh)  * gainHigh
//
// With all gains at 1 the LR4 recombination is all-pass (flat magnitude);
// a gain of 0 is a true digital kill that does not bleed into the other
// bands except inside the +-1 octave transition region. Crossover
// coefficients are fixed for a given sample rate (computed once, before
// the audio device starts); only the smoothed band gains move per block.
//
// Knob feel (user decision 2026-10-05, Traktor-style): 0.5 = flat,
// full CCW = kill, full CW = +6 dB boost - see curve(). The UI writes
// targets through Mixer (atomics); this class smooths them over ~12 ms
// on the audio thread. No locks, no allocation, no IO, no exceptions
// (SPEC §4.7).
//
// Audio-thread-only state (plain members): smoothed knobs and biquad
// history. setSampleRate()/setTarget() are called only before the device
// starts (or from the UI thread while the audio thread only ever touches
// the targets through Mixer's relaxed atomics).
class Eq3 {
public:
    static constexpr int kBands = 3; // 0 = low, 1 = mid, 2 = high

    // Pure knob curve: 0.5 -> 1.0 (flat), 0 -> 0 (kill), 1 -> +6 dB.
    static float curve(float knob) noexcept;

    Eq3() noexcept; // 44.1 kHz crossover coefficients

    // Recomputes the crossover coefficients; call before the device starts.
    void setSampleRate(uint32_t fs) noexcept;

    // Knob target for a band, clamped to 0..1.
    void setTarget(int band, float knob) noexcept;

    // Smooth the knobs toward their targets (one-pole, ~12 ms) and refresh
    // the band gains. Called once per block before any samples.
    void beginBlock(uint32_t frameCount) noexcept;

    // One sample through the crossover split: channel 0 = L, 1 = R.
    float process(int channel, float x) noexcept;

private:
    struct Biquad {
        // Coefficients normalized by a0 (direct form 1).
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        struct State {
            float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
        };
        State st[2]; // per channel

        void set(float nb0, float nb1, float nb2, float na0, float na1,
                 float na2) noexcept;
        float run(int channel, float x) noexcept;
    };

    void updateCrossover() noexcept;

    Biquad lpLow_[2];  // LR4 low-pass @ fcLow
    Biquad hpLow_[2];  // LR4 high-pass @ fcLow
    Biquad lpHigh_[2]; // LR4 low-pass @ fcHigh (on the HP4(fcLow) path)
    Biquad hpHigh_[2]; // LR4 high-pass @ fcHigh (on the HP4(fcLow) path)

    float target_[kBands] = {0.5f, 0.5f, 0.5f};
    float smooth_[kBands] = {0.5f, 0.5f, 0.5f};
    float gain_[kBands] = {1.0f, 1.0f, 1.0f};
    uint32_t fs_ = 44100;
};

} // namespace tng
