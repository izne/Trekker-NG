#pragma once

#include "interpolate.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sde {

// One stereo stem: interleaved float32 L/R, fully decoded to RAM (SPEC §4.2).
// An empty `data` (or frames == 0) means "missing stem = silent" (SPEC §5).
struct Stem {
    std::string name;
    uint32_t color = 0xFFFFFFFF; // 0xAARRGBB
    std::vector<float> data;
    int64_t frames = 0;
};

// Immutable, fully prepared track data. Built off-thread by the loader and
// only ever read by the audio thread (SPEC §4.7).
struct DeckData {
    std::string title;
    std::string artist;
    float bpm = 0.0f; // 0 = unknown
    double firstBeatOffsetMs = 0.0;
    uint32_t sampleRate = 0;
    int64_t frames = 0; // all stems padded to this length
    std::array<Stem, 4> stems;
};

// A deck: 4 stems sharing one playhead (SPEC §4.2).
//
// Realtime design (SPEC §4.7):
//  - UI -> audio: only atomics (targets, rate, seek request); never a lock.
//  - Audio-thread-only state: playhead, smoothed rate, gains, crossfade.
//    It is written by setTrack()/resetPosition() only while the device is
//    stopped; during playback only render() touches it.
//
// Smoothing (M2, SPEC §4.3): the UI writes a *target* rate; render() glides
// the effective rate toward it with a per-sample one-pole (~15 ms), so rate
// changes are continuous instead of stepping once per block.
class Deck {
public:
    static constexpr int kStemCount = 4;

    Deck();

    // --- audio thread ---
    void render(float* outInterleaved, uint32_t frameCount) noexcept;

    // --- console/UI thread ---
    void setTrack(std::unique_ptr<DeckData> data); // device must be stopped
    const DeckData* track() const { return data_.get(); }

    void setPlaying(bool playing) noexcept;
    bool playing() const noexcept { return playing_.load(std::memory_order_acquire); }

    void toggleStem(int index) noexcept;
    void setStem(int index, bool on) noexcept;
    void soloStem(int index) noexcept;
    void setAllStems(bool on) noexcept;
    bool stemOn(int index) const noexcept;

    void setRate(double rate) noexcept; // sets the target; display uses this
    double rate() const noexcept { return rateTarget_.load(std::memory_order_relaxed); }

    void setInterp(InterpMode mode) noexcept;
    InterpMode interp() const noexcept;

    void resetPosition() noexcept; // device must be stopped
    // Thread-safe playhead jump: queued as an atomic request and consumed by
    // the audio thread at the start of the next render block. While audible
    // the jump is declicked with a ~2 ms crossfade (SPEC §4.5).
    void requestSeek(int64_t frame) noexcept;
    double positionFrames() const noexcept; // last rendered playhead (for display)

private:
    void zero(float* out, uint32_t count) const noexcept;

    std::unique_ptr<DeckData> data_;

    // UI-written atomics.
    std::atomic<bool> playing_{false};
    std::atomic<double> rateTarget_{1.0};
    std::atomic<uint8_t> stemTarget_[kStemCount]; // 0 or 1
    std::atomic<int> interp_{static_cast<int>(InterpMode::Hermite)};
    std::atomic<double> displayPos_{0.0};
    std::atomic<int64_t> seekRequest_{-1}; // -1 = none; consumed by audio thread

    // Audio-thread-only state (setTrack() precomputes the constants).
    double playhead_ = 0.0;
    float stemGain_[kStemCount] = {0, 0, 0, 0};
    float outputGain_ = 0.0f; // transport fade: 0 = silent/paused settled
    float rampStep_ = 0.0f;   // 1 / (5 ms * sampleRate): stem + transport ramps
    double rateSmoothed_ = 1.0;
    double rateCoeff_ = 1.0;    // one-pole coefficient, 1 - exp(-1 / (tau * fs))
    int64_t declickFrames_ = 1; // ~2 ms seek crossfade length, in frames
    int64_t xfadeRemaining_ = 0;
    double xfadeOldPos_ = 0.0; // pre-jump position feeding the crossfade
};

} // namespace sde
