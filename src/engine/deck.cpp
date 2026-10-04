#include "deck.h"

#include <cstring>

namespace sde {

namespace {
// Click-free stem toggles: ramp the applied gain to its target over ~5 ms
// (SPEC §4.4).
constexpr double kRampSeconds = 0.005;
} // namespace

Deck::Deck() {
    for (int i = 0; i < kStemCount; ++i) {
        stemTarget_[i].store(1, std::memory_order_relaxed); // all on by default
    }
}

void Deck::zero(float* out, uint32_t count) const noexcept {
    std::memset(out, 0, count * sizeof(float));
}

void Deck::setTrack(std::unique_ptr<DeckData> data) {
    // Contract: audio device is stopped here, so touching the non-atomic
    // playhead/gains is safe.
    data_ = std::move(data);
    playhead_ = 0.0;
    displayPos_.store(0.0, std::memory_order_relaxed);
    playing_.store(false, std::memory_order_release);
    for (int i = 0; i < kStemCount; ++i) {
        // Snap gains to the targets so a load never produces a fade-in click.
        stemGain_[i] = stemTarget_[i].load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    }
}

void Deck::setPlaying(bool playing) noexcept {
    playing_.store(playing, std::memory_order_release);
}

void Deck::toggleStem(int index) noexcept {
    if (index < 0 || index >= kStemCount) return;
    const uint8_t target = stemTarget_[index].load(std::memory_order_relaxed);
    stemTarget_[index].store(target ? 0 : 1, std::memory_order_relaxed);
}

void Deck::setStem(int index, bool on) noexcept {
    if (index < 0 || index >= kStemCount) return;
    stemTarget_[index].store(on ? 1 : 0, std::memory_order_relaxed);
}

void Deck::soloStem(int index) noexcept {
    if (index < 0 || index >= kStemCount) return;
    for (int i = 0; i < kStemCount; ++i) {
        stemTarget_[i].store(i == index ? 1 : 0, std::memory_order_relaxed);
    }
}

void Deck::setAllStems(bool on) noexcept {
    for (int i = 0; i < kStemCount; ++i) {
        stemTarget_[i].store(on ? 1 : 0, std::memory_order_relaxed);
    }
}

bool Deck::stemOn(int index) const noexcept {
    if (index < 0 || index >= kStemCount) return false;
    return stemTarget_[index].load(std::memory_order_relaxed) != 0;
}

void Deck::setRate(double rate) noexcept {
    // Safety clamp far beyond the ±16% pitch range (SPEC §4.3); prevents a
    // runaway playhead from a bad value.
    if (rate < 0.05) rate = 0.05;
    if (rate > 2.0) rate = 2.0;
    rate_.store(rate, std::memory_order_relaxed);
}

void Deck::setInterp(InterpMode mode) noexcept {
    interp_.store(static_cast<int>(mode), std::memory_order_relaxed);
}

InterpMode Deck::interp() const noexcept {
    return static_cast<InterpMode>(interp_.load(std::memory_order_relaxed));
}

void Deck::resetPosition() noexcept {
    playhead_ = 0.0;
    displayPos_.store(0.0, std::memory_order_relaxed);
}

void Deck::requestSeek(int64_t frame) noexcept {
    if (frame < 0) frame = 0;
    seekRequest_.store(frame, std::memory_order_release);
}

double Deck::positionFrames() const noexcept {
    return displayPos_.load(std::memory_order_relaxed);
}

void Deck::render(float* out, uint32_t frameCount) noexcept {
    // Consume a pending playhead jump first, even while paused, so the
    // position display follows immediately (SPEC §4.5 declick arrives in M2).
    const int64_t seek = seekRequest_.exchange(-1, std::memory_order_acq_rel);
    if (seek >= 0) {
        playhead_ = static_cast<double>(seek);
        displayPos_.store(playhead_, std::memory_order_relaxed);
    }

    const DeckData* d = data_.get();
    if (d == nullptr || d->frames <= 0 || !playing_.load(std::memory_order_acquire)) {
        zero(out, frameCount * 2);
        displayPos_.store(playhead_, std::memory_order_relaxed);
        return;
    }

    const double rate = rate_.load(std::memory_order_relaxed);
    const InterpMode mode = interp();
    // Per-sample linear ramp toward the target gain (SPEC §4.4).
    const float rampStep =
        static_cast<float>(1.0 / (kRampSeconds * static_cast<double>(d->sampleRate)));

    uint32_t i = 0;
    for (; i < frameCount; ++i) {
        if (playhead_ >= static_cast<double>(d->frames)) {
            playhead_ = static_cast<double>(d->frames);
            break; // end of track: stop (SPEC §4.2)
        }
        if (playhead_ < 0.0) playhead_ = 0.0; // clamp at start when reversing

        float left = 0.0f;
        float right = 0.0f;
        for (int s = 0; s < kStemCount; ++s) {
            const float target = stemTarget_[s].load(std::memory_order_relaxed) ? 1.0f : 0.0f;
            float g = stemGain_[s];
            if (g < target) {
                g += rampStep;
                if (g > target) g = target;
            } else if (g > target) {
                g -= rampStep;
                if (g < target) g = target;
            }
            stemGain_[s] = g;

            const Stem& stem = d->stems[s];
            if (g <= 0.0f || stem.frames == 0) continue;

            // One shared playhead for all 4 stems => sample-locked, zero drift.
            left += readFrame(stem.data.data(), stem.frames, 2, 0, playhead_, mode) * g;
            right += readFrame(stem.data.data(), stem.frames, 2, 1, playhead_, mode) * g;
        }
        out[i * 2] = left;
        out[i * 2 + 1] = right;
        playhead_ += rate;
    }
    if (i < frameCount) {
        zero(out + i * 2, (frameCount - i) * 2);
    }
    if (playhead_ >= static_cast<double>(d->frames)) {
        playing_.store(false, std::memory_order_release);
    }
    displayPos_.store(playhead_, std::memory_order_relaxed);
}

} // namespace sde
