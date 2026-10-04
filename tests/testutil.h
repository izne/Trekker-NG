#pragma once

// Shared helpers for the engine tests (SPEC §9). Everything is synthesized
// in memory - no Python, no files, no soundcard (see docs/DECISIONS.md).

#include "deck.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rtcheck {
// Defined in test_main.cpp: allocation counter for the realtime-safety test
// (SPEC §9.6).
extern thread_local int inScope;
extern thread_local long allocCount;

struct Scope {
    Scope() {
        ++inScope;
        allocCount = 0;
    }
    ~Scope() { --inScope; }
    long count() const { return allocCount; }
};
} // namespace rtcheck

namespace tutil {

constexpr double kPi = 3.14159265358979323846;

// DeckData with one sine stem per entry in `freqs` (freq <= 0 => silent stem).
// Stems beyond freqs.size() stay empty (SPEC §5: missing stems are silent).
inline std::unique_ptr<sde::DeckData> makeSineTrack(uint32_t fs, double seconds,
                                                    const std::vector<double>& freqs,
                                                    double amp = 0.5) {
    auto d = std::make_unique<sde::DeckData>();
    d->sampleRate = fs;
    d->frames = static_cast<int64_t>(seconds * fs);
    for (size_t s = 0; s < freqs.size() && s < 4; ++s) {
        sde::Stem& st = d->stems[s];
        st.name = "sine" + std::to_string(s + 1);
        st.frames = d->frames;
        st.data.resize(static_cast<size_t>(d->frames) * 2);
        if (freqs[s] <= 0.0) continue;
        for (int64_t i = 0; i < d->frames; ++i) {
            const float v =
                static_cast<float>(amp * std::sin(2.0 * kPi * freqs[s] * i / fs));
            st.data[static_cast<size_t>(i) * 2] = v;
            st.data[static_cast<size_t>(i) * 2 + 1] = v;
        }
    }
    return d;
}

// `stemCount` stems with identical impulse (click) trains, one every
// `periodFrames` - the SPEC §9.2 stem-lock probe.
inline std::unique_ptr<sde::DeckData> makeClickTrack(uint32_t fs, double seconds,
                                                     int64_t periodFrames,
                                                     int stemCount = 2) {
    auto d = std::make_unique<sde::DeckData>();
    d->sampleRate = fs;
    d->frames = static_cast<int64_t>(seconds * fs);
    for (int s = 0; s < stemCount && s < 4; ++s) {
        sde::Stem& st = d->stems[s];
        st.name = "click" + std::to_string(s + 1);
        st.frames = d->frames;
        st.data.assign(static_cast<size_t>(d->frames) * 2, 0.0f);
        for (int64_t i = 0; i < d->frames; i += periodFrames) {
            st.data[static_cast<size_t>(i) * 2] = 1.0f;
            st.data[static_cast<size_t>(i) * 2 + 1] = 1.0f;
        }
    }
    return d;
}

// Renders `n` frames (in chunks) and returns the interleaved stereo output.
inline std::vector<float> renderFrames(sde::Deck& deck, int64_t n, uint32_t chunk = 256) {
    std::vector<float> out(static_cast<size_t>(n) * 2, 0.0f);
    int64_t done = 0;
    while (done < n) {
        const uint32_t c = static_cast<uint32_t>(std::min<int64_t>(chunk, n - done));
        deck.render(out.data() + static_cast<size_t>(done) * 2, c);
        done += c;
    }
    return out;
}

// Largest sample-to-sample jump on one channel in [a, b). A click-free ramp
// keeps this near the waveform's own derivative; a hard toggle/seek shows up
// as a jump of the signal's full amplitude.
inline float maxStep(const std::vector<float>& x, int64_t a, int64_t b, int ch = 0) {
    float m = 0.0f;
    for (int64_t i = std::max<int64_t>(a, 1); i < b; ++i) {
        const float d = std::fabs(x[static_cast<size_t>(i) * 2 + ch] -
                                  x[static_cast<size_t>(i - 1) * 2 + ch]);
        if (d > m) m = d;
    }
    return m;
}

// Zero-crossing frequency estimate over frames [a, b) of one channel
// (SPEC §9.1 style measurement; needs a quasi-sine, not music).
inline double measureFreq(const std::vector<float>& x, int64_t a, int64_t b, uint32_t fs,
                          int ch = 0) {
    long crossings = 0;
    bool prev = x[static_cast<size_t>(a) * 2 + ch] >= 0.0f;
    for (int64_t i = a + 1; i < b; ++i) {
        const bool s = x[static_cast<size_t>(i) * 2 + ch] >= 0.0f;
        if (s != prev) ++crossings;
        prev = s;
    }
    return static_cast<double>(crossings) / (2.0 * static_cast<double>(b - a) /
                                             static_cast<double>(fs));
}

} // namespace tutil
