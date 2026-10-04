#pragma once

// The M3 deck view (SPEC §6/§8): title, composite waveform overview on top
// (full width), four per-stem lanes below it (colored, dimmed when muted),
// playhead + click-seek on every lane, 4 colored stem toggles, transport,
// reversed vertical pitch fader, time/beat readout and the load box.

#include "deck.h"

#include <array>
#include <cstdint>
#include <vector>

namespace tui {

// Min/max peak columns, precomputed once per track on the UI thread
// (SPEC §6 "precomputed min/max peaks"); the audio thread never touches these.
struct WaveformCache {
    struct Lane {
        std::vector<float> mins;
        std::vector<float> maxs;
        float peak = 1.0f; // auto-scale: largest |sample| in this lane
        bool present = false;
    };

    Lane composite;                 // mixed signal, top row, full width
    std::array<Lane, 4> stems;      // one lane per stem
    void build(const tng::DeckData& data, int bins = 1024);
};

class DeckView {
public:
    // Draws the whole deck panel. Returns a non-empty path when the user
    // pressed Load with a path in the box; the caller starts the async load.
    const char* draw(tng::Deck& deck);

    // Call after publishTrack(): forces the waveform rebuild for the new track.
    void onTrackChanged() { wfDirty_ = true; }

private:
    // One full-width waveform row. `color` includes the mute dimming,
    // `label` is drawn over the lane (may be null).
    void drawLane(tng::Deck& deck, const tng::DeckData& data, const WaveformCache::Lane& lane,
                  uint32_t color, float height, const char* label);

    WaveformCache wf_;
    bool wfDirty_ = true; // rebuild on next draw
    char pathBuf_[1024] = {};
    float pitchPct_ = 0.0f; // mirrors the deck rate while the fader is idle
};

} // namespace tui
