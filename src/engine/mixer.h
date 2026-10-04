#pragma once

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
// Single-deck mode: `deckB == nullptr` bypasses the crossfader and line B -
// deckA passes at line-gain A. This keeps the M1-M3 console and the M3 UI at
// their original loudness until the two-deck UI arrives.
class Mixer {
public:
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

private:
    std::atomic<float> lineA_{1.0f};
    std::atomic<float> lineB_{1.0f};
    std::atomic<float> crossfader_{0.0f}; // centered = both decks audible
    std::atomic<float> masterGain_{0.7f}; // M1/M2 default until the master UI

    // Audio-thread-only ramp state. Starts at 1.0 so single-deck mode (the
    // console and the pre-M4c UI) is steady from the very first sample -
    // the first two-deck block ramps to the equal-power center gains.
    float rampA_ = 1.0f;
    float rampB_ = 1.0f;
};

} // namespace tng
