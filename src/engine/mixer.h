#pragma once

#include "eq.h"

#include <atomic>
#include <cstdint>

namespace tng {

// Mixer (SPEC §4.6): per-deck line gains, crossfader, master gain and a
// safety clamp. process() runs inside the audio callback: no allocation,
// no locks, no IO, no exceptions (SPEC §4.7) - just atomic loads, one
// cos/sin pair per block, multiplies and a clamp.
//
// Crossfader curve: M4 ships a fixed **equal-power** curve (cos/sin, so the
// blended power stays constant while switching); the selectable curves
// (linear / constant-power / sharp-cut) are M5 per the milestone table.
//
// The line/cross gains ramp linearly across the block toward their atomic
// targets (audio-thread-only state, same idea as the deck's transport ramp),
// so fader moves never step the gain mid-block (no zipper noise).
//
// M5a: the per-deck 3-band EQ (eq.h) sits between the deck render and the
// crossfade - the UI writes atomic knobs, the audio thread smooths them and
// rebuilds the biquads in process().
//
// Single-deck mode: `deckB == nullptr` bypasses the crossfader, line B and
// the M5a EQ - deckA passes at line-gain A, byte-identical to the pre-M5a
// console render. The live UI always registers both decks (the two-deck
// path carries the EQ in both UI modes).
class Mixer {
public:
    Mixer() noexcept; // flat EQ knobs (0.5) on both decks

    // deckA may alias out (the console's offline render mixes in place:
    // each sample is read before it is written). deckB may be null.
    void process(const float* deckA, const float* deckB, float* out,
                 uint32_t frameCount) noexcept;

    void setLineGain(int deck, float gain) noexcept; // deck 0 = A, 1 = B; 0..1
    float lineGain(int deck) const noexcept;
    void setCrossfader(float position) noexcept; // -1 = A only, +1 = B only
    float crossfader() const noexcept;
    void setMasterGain(float gain) noexcept;
    float masterGain() const noexcept;

    // M5a 3-band EQ: band 0 = low, 1 = mid, 2 = high; knob 0..1 (0.5 =
    // flat, 0 = kill, 1 = +6 dB). UI thread writes, audio thread reads
    // (relaxed) every block.
    void setEqKnob(int deck, int band, float knob) noexcept;
    float eqKnob(int deck, int band) const noexcept;

    // Feeds the EQ smoothing time; call before the device starts.
    void setSampleRate(uint32_t fs) noexcept;

private:
    std::atomic<float> lineA_{1.0f};
    std::atomic<float> lineB_{1.0f};
    std::atomic<float> crossfader_{0.0f}; // centered = both decks audible
    std::atomic<float> masterGain_{0.7f}; // M1/M2 default until the master UI
    std::atomic<float> eqKnob_[2][Eq3::kBands];

    // Audio-thread-only state. Starts at 1.0 so single-deck mode (the
    // console and the pre-M4c UI) is steady from the very first sample -
    // the first two-deck block ramps to the equal-power center gains.
    float rampA_ = 1.0f;
    float rampB_ = 1.0f;
    Eq3 eqA_, eqB_;
};

} // namespace tng
