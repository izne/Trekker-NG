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
// Threading contract (M1): setTrack() may only be called while the audio
// device is stopped. Everything else is lock-free via atomics and safe to
// call from the UI/console thread while audio runs (SPEC §4.4/§4.7).
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

    void setRate(double rate) noexcept;
    double rate() const noexcept { return rate_.load(std::memory_order_relaxed); }

    void setInterp(InterpMode mode) noexcept;
    InterpMode interp() const noexcept;

    void resetPosition() noexcept; // device must be stopped
    // Thread-safe playhead jump: queued as an atomic request and consumed by
    // the audio thread at the start of the next render block (no locks).
    void requestSeek(int64_t frame) noexcept;
    double positionFrames() const noexcept; // last rendered playhead (for display)

private:
    void zero(float* out, uint32_t count) const noexcept;

    std::unique_ptr<DeckData> data_;

    std::atomic<bool> playing_{false};
    std::atomic<double> rate_{1.0};
    std::atomic<uint8_t> stemTarget_[kStemCount]; // 0 or 1, written by UI thread
    std::atomic<int> interp_{static_cast<int>(InterpMode::Hermite)};
    std::atomic<double> displayPos_{0.0};
    std::atomic<int64_t> seekRequest_{-1}; // -1 = none; consumed by audio thread

    // Audio-thread-only state (never touched while the device is stopped).
    double playhead_ = 0.0;
    float stemGain_[kStemCount] = {0, 0, 0, 0}; // ramped applied gains
};

} // namespace sde
