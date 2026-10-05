#pragma once

// The M3 deck view (SPEC §6/§8): title, composite waveform overview on top
// (full width), four per-stem lanes below it (colored, dimmed when muted),
// playhead + click-seek on every lane, 4 colored stem toggles, transport,
// reversed vertical pitch fader, time/beat readout and the load box.

#include "deck.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
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
    // A nudge survives a hot swap: the rate target carries over with the
    // transport state, so the base pitch it will restore stays valid.
    void onTrackChanged() {
        wfDirty_ = true;
        mainCueFrame_ = 0; // the old cue points at the previous track's frames
        cuePreviewing_ = false;
    }

    // M4d: the path of the currently loaded track (set by main.cpp after each
    // successful load); hot-cue edits persist through saveCues() with it.
    void setTrackPath(std::filesystem::path path) { trackPath_ = std::move(path); }

    // M4d: one-shot status message for main.cpp (cue save results); returns
    // false when there is nothing pending.
    bool takeNotice(std::string& out, bool& err);

    // M4d: the fader's pitch range (SPEC §4.3 selector, session-only toggle).
    float pitchRange() const { return pitchRange_; }

    // M4d: true while a Shift+click nudge owns the deck rate (pitch keys back
    // off so they cannot stomp the base pitch the nudge restores).
    bool nudging() const { return nudging_; }
    // M4d: abort a nudge (window focus loss): restores the base pitch on the
    // deck. No-op when no nudge is active.
    void cancelNudge(tng::Deck& deck);

private:
    // One full-width waveform row. `color` includes the mute dimming and
    // paints the waveform; `label` is drawn over the lane in white (may be
    // null).
    void drawLane(tng::Deck& deck, const tng::DeckData& data, const WaveformCache::Lane& lane,
                  uint32_t color, float height, const char* label);

    // M4d: persist the deck's hot cues after a set/clear; the result becomes
    // the pending notice for the status line.
    void persistCues(const tng::Deck& deck, const char* what);

    WaveformCache wf_;
    bool wfDirty_ = true; // rebuild on next draw
    char pathBuf_[1024] = {};
    float pitchPct_ = 0.0f; // mirrors the deck rate while the fader is idle
    float vuLevel_ = 0.0f;  // smoothed VU bar: peak-hold with a per-frame fall

    // --- M4d state (per deck view) ---
    int64_t mainCueFrame_ = 0;  // main cue (SPEC §4.5), session-only, 0 = start
    bool cuePreviewing_ = false; // CUE held: previewing, return on release
    float pitchRange_ = 10.0f;  // SPEC §4.3 selector: 10 or 16 (session-only)
    bool nudging_ = false;      // Shift+click nudge active on the fader
    float nudgeBase_ = 0.0f;    // pitchPct_ frozen at the nudge press
    float nudgeTarget_ = 0.0f;  // bent pitch while the mouse is held
    std::filesystem::path trackPath_; // saveCues target for hot-cue edits
    std::string notice_;        // pending status line for main.cpp
    bool noticeErr_ = false;
};

} // namespace tui
