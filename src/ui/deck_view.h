#pragma once

// The M3 deck view (SPEC §6/§8): title, composite waveform overview on top
// (full width), four per-stem lanes below it (colored, dimmed when muted),
// playhead + click-seek on every lane, 4 colored stem toggles, transport,
// reversed vertical pitch fader, time/beat readout and the load box.

#include "deck.h"

#include <array>
#include <cstdint>
#include <vector>

struct ImFont; // Dear ImGui, global scope (imgui.h)

namespace tui {

// M4display: DSEG7 VFD readout font, loaded once in main.cpp (null until
// the first frame; the readouts fall back to the default font without it).
extern ImFont* vfdFont;
constexpr float kVfdFontSize = 15.0f;

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
    // `active` (M4c) highlights the title: it is the deck the global keys
    // (Space, pitch) currently target.
    const char* draw(tng::Deck& deck, bool active);

    // Call after publishTrack(): forces the waveform rebuild for the new track.
    void onTrackChanged() { wfDirty_ = true; }

private:
    // One full-width waveform row. `color` includes the mute dimming and
    // paints the waveform; `label` is drawn over the lane in white (may be
    // null).
    void drawLane(tng::Deck& deck, const tng::DeckData& data, const WaveformCache::Lane& lane,
                  uint32_t color, float height, const char* label);

    WaveformCache wf_;
    bool wfDirty_ = true; // rebuild on next draw
    char pathBuf_[1024] = {};
    float pitchPct_ = 0.0f; // mirrors the deck rate while the fader is idle
    float vuLevel_ = 0.0f;  // smoothed VU bar: peak-hold with a per-frame fall
};

} // namespace tui
