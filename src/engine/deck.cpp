#include "deck.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tng {

namespace {
// Click-free transitions (SPEC §4.4/§4.5):
//  - stem mutes and play/pause: ramp over ~5 ms
//  - playhead jumps: crossfade over ~2 ms
//  - rate changes: one-pole glide, time constant ~15 ms (SPEC §4.3)
constexpr double kRampSeconds = 0.005;
constexpr double kDeclickSeconds = 0.002;
constexpr double kRateTauSeconds = 0.015;
} // namespace

Deck::Deck() {
    for (int i = 0; i < kStemCount; ++i) {
        stemTarget_[i].store(1, std::memory_order_relaxed); // all on by default
    }
}

Deck::~Deck() {
    for (DeckData* d : retired_) delete d; // pending swaps: no audio thread here
}

void Deck::zero(float* out, uint32_t count) const noexcept {
    std::memset(out, 0, count * sizeof(float));
}

void Deck::setTrack(std::unique_ptr<DeckData> data) {
    // Contract: audio device is stopped here, so touching the non-atomic
    // audio-thread state is safe.
    if (data_) retired_.push_back(data_.release()); // freed by drainRetired()/~Deck
    data_ = std::move(data);
    dataPub_.store(data_.get(), std::memory_order_release);
    playhead_ = 0.0;
    displayPos_.store(0.0, std::memory_order_relaxed);
    playing_.store(false, std::memory_order_release);
    seekRequest_.store(-1, std::memory_order_relaxed);
    clearLoop(); // points belong to the previous track

    const uint32_t fs = data_ ? data_->sampleRate : 44100;
    rampStep_ = static_cast<float>(1.0 / (kRampSeconds * static_cast<double>(fs)));
    declickFrames_ = std::max<int64_t>(1, static_cast<int64_t>(kDeclickSeconds * fs));
    rateCoeff_ = 1.0 - std::exp(-1.0 / (kRateTauSeconds * static_cast<double>(fs)));
    rateSmoothed_ = rateTarget_.load(std::memory_order_relaxed);
    outputGain_ = 0.0f; // start silent: first play fades in (no onset click)
    xfadeRemaining_ = 0;
    for (int i = 0; i < kStemCount; ++i) {
        // Snap gains to the targets so a load never produces a fade-in click.
        stemGain_[i] = stemTarget_[i].load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    }
}

void Deck::publishTrack(std::unique_ptr<DeckData> data) {
    // Hot path (SPEC §4.7): only the pointer swap happens here - no touching
    // audio-thread state. The audio thread adopts `data` at the start of its
    // next render (position resets, transport/stems/rate carry over). The old
    // data is retired and freed later on this non-audio thread.
    if (data_) retired_.push_back(data_.release());
    data_ = std::move(data);
    dataPub_.store(data_.get(), std::memory_order_release);
    clearLoop(); // loop points reference the old track's positions (cues,
                  // which live in DeckData, swap with the track instead)
}

void Deck::drainRetired() {
    if (retired_.empty()) return;
    // Safe only once the audio thread has adopted the current pointer: any
    // block that could still reference retired data has then completed
    // (renders are sequential on the single audio thread).
    if (acked_.load(std::memory_order_acquire) != dataPub_.load(std::memory_order_acquire)) {
        return;
    }
    for (DeckData* d : retired_) {
        delete d;
    }
    retired_.clear();
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
    rateTarget_.store(rate, std::memory_order_relaxed);
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

bool Deck::setLoop(int64_t inFrame, int64_t outFrame) noexcept {
    if (inFrame < 0 || outFrame <= inFrame) return false;
    loopIn_.store(inFrame, std::memory_order_relaxed);
    loopOut_.store(outFrame, std::memory_order_relaxed);
    return true;
}

bool Deck::setLoopIn(int64_t frame) noexcept {
    if (frame < 0) return false;
    const int64_t out = loopOut_.load(std::memory_order_relaxed);
    if (out >= 0 && frame >= out) return false;
    loopIn_.store(frame, std::memory_order_relaxed);
    return true;
}

bool Deck::setLoopOut(int64_t frame) noexcept {
    if (frame < 0) return false;
    const int64_t in = loopIn_.load(std::memory_order_relaxed);
    if (in >= 0 && frame <= in) return false;
    loopOut_.store(frame, std::memory_order_relaxed);
    return true;
}

bool Deck::setQuickLoop(int beats) noexcept {
    if (beats <= 0 || !data_ || data_->bpm <= 0.0f) return false;
    const double fs = static_cast<double>(std::max(data_->sampleRate, 1u));
    int64_t in = static_cast<int64_t>(positionFrames());
    if (in < 0) in = 0;
    int64_t beat = static_cast<int64_t>(60.0 / static_cast<double>(data_->bpm) * fs + 0.5);
    if (beat < 1) beat = 1;
    int64_t out = in + beat * static_cast<int64_t>(beats);
    if (out > data_->frames) out = data_->frames; // clamp at the track end
    if (!setLoop(in, out)) return false;
    loopActive_.store(true, std::memory_order_relaxed);
    return true;
}

void Deck::clearLoop() noexcept {
    loopIn_.store(-1, std::memory_order_relaxed);
    loopOut_.store(-1, std::memory_order_relaxed);
    loopActive_.store(false, std::memory_order_relaxed);
}

bool Deck::setLoopActive(bool active) noexcept {
    if (active) {
        const int64_t in = loopIn_.load(std::memory_order_relaxed);
        const int64_t out = loopOut_.load(std::memory_order_relaxed);
        if (in < 0 || out <= in) return false;
    }
    loopActive_.store(active, std::memory_order_relaxed);
    return true;
}

bool Deck::loopActive() const noexcept {
    return loopActive_.load(std::memory_order_relaxed);
}

int64_t Deck::loopIn() const noexcept {
    return loopIn_.load(std::memory_order_relaxed);
}

int64_t Deck::loopOut() const noexcept {
    return loopOut_.load(std::memory_order_relaxed);
}

bool Deck::setHotCue(int slot, double positionMs) noexcept {
    if (!data_ || slot < 0) return false;
    std::vector<Cue>& cues = data_->cues;
    if (positionMs < 0.0) positionMs = 0.0;
    if (slot == static_cast<int>(cues.size())) {
        if (cues.size() >= 8) return false; // the UI offers 8 slots (SPEC §4.5)
        Cue c;
        c.name = "cue " + std::to_string(slot + 1);
        c.positionMs = positionMs;
        cues.push_back(std::move(c));
        return true;
    }
    if (slot > static_cast<int>(cues.size())) return false;
    cues[static_cast<size_t>(slot)].positionMs = positionMs;
    return true;
}

bool Deck::clearHotCue(int slot) noexcept {
    if (!data_ || slot < 0 || slot >= static_cast<int>(data_->cues.size())) return false;
    data_->cues.erase(data_->cues.begin() + slot);
    return true;
}

void Deck::render(float* out, uint32_t frameCount) noexcept {
    // Consume a pending playhead jump first, even while paused, so the
    // position display follows immediately.
    const int64_t seek = seekRequest_.exchange(-1, std::memory_order_acq_rel);

    const DeckData* d = dataPub_.load(std::memory_order_acquire);
    const bool adopted = d != acked_.load(std::memory_order_acquire);
    if (adopted) {
        // Adopt newly published data (or none): transport resets happen on
        // the audio thread only; so do the sample-rate-derived constants.
        // Nothing is freed here - SPEC §4.7.
        playhead_ = 0.0;
        xfadeRemaining_ = 0;
        seekRequest_.store(-1, std::memory_order_relaxed);
        if (d != nullptr && d->sampleRate > 0) {
            const double fs = static_cast<double>(d->sampleRate);
            rampStep_ = static_cast<float>(1.0 / (kRampSeconds * fs));
            declickFrames_ = std::max<int64_t>(1, static_cast<int64_t>(kDeclickSeconds * fs));
            rateCoeff_ = 1.0 - std::exp(-1.0 / (kRateTauSeconds * fs));
        }
        displayPos_.store(0.0, std::memory_order_relaxed);
        acked_.store(d, std::memory_order_release);
    }

    if (d == nullptr || d->frames <= 0) {
        zero(out, frameCount * 2);
        displayPos_.store(playhead_, std::memory_order_relaxed);
        peak_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const bool playing = playing_.load(std::memory_order_acquire);
    const double rateTarget = rateTarget_.load(std::memory_order_relaxed);
    const InterpMode mode = interp();

    // Loop (SPEC §4.5): snapshot the points once per block; the wrap itself
    // is checked per sample below so it lands on the exact crossing frame.
    int64_t loopLi = loopIn_.load(std::memory_order_relaxed);
    int64_t loopLo = loopOut_.load(std::memory_order_relaxed);
    bool loopOn = loopActive_.load(std::memory_order_relaxed);
    double loopInF = 0.0;
    double loopOutF = 0.0;
    if (loopOn) {
        if (loopLo > d->frames) loopLo = d->frames;
        if (loopLi < 0 || loopLo <= loopLi) {
            loopOn = false; // stale points against this track
        } else {
            loopInF = static_cast<double>(loopLi);
            loopOutF = static_cast<double>(loopLo);
        }
    }

    if (seek >= 0 && !adopted) { // a seek racing a publish targets the old track: drop
        if (outputGain_ > 0.0f) {
            // Audible jump: declick with a crossfade from the old position.
            // A jump during an active crossfade only retargets the new path.
            if (xfadeRemaining_ <= 0) xfadeOldPos_ = playhead_;
            xfadeRemaining_ = declickFrames_;
        }
        // Silent jump (paused/settled): plain move, nothing to click.
        playhead_ = static_cast<double>(seek);
    }

    // Sums the 4 stems at `pos` with the current (already ramped) gains.
    auto mix = [&](double pos, float& l, float& r) {
        l = 0.0f;
        r = 0.0f;
        for (int s = 0; s < kStemCount; ++s) {
            const float g = stemGain_[s];
            const Stem& stem = d->stems[s];
            if (g <= 0.0f || stem.frames == 0) continue;
            // One shared playhead for all 4 stems => sample-locked, zero drift.
            l += readFrame(stem.data.data(), stem.frames, 2, 0, pos, mode) * g;
            r += readFrame(stem.data.data(), stem.frames, 2, 1, pos, mode) * g;
        }
    };

    // Loop wrap (SPEC §4.5): the same ~2 ms declick as a seek - the old path
    // continues just past the loop end while the new one starts at loopIn.
    auto wrapLoop = [&]() {
        if (outputGain_ > 0.0f) { // silent jump: plain move, nothing to click
            if (xfadeRemaining_ <= 0) xfadeOldPos_ = playhead_;
            xfadeRemaining_ = declickFrames_;
        }
        double over = playhead_ - loopOutF;
        if (over < 0.0) over = 0.0;
        const double len = loopOutF - loopInF;
        if (len > 0.0) over = std::fmod(over, len); // activation far past out
        playhead_ = loopInF + over;
    };

    uint32_t i = 0;
    float peak = 0.0f; // block peak |sample| for the VU meter (no extra pass)
    for (; i < frameCount; ++i) {
        // Transport fade toward play/pause - kills the pause/resume click.
        const float ogTarget = playing ? 1.0f : 0.0f;
        if (outputGain_ < ogTarget) {
            outputGain_ += rampStep_;
            if (outputGain_ > ogTarget) outputGain_ = ogTarget;
        } else if (outputGain_ > ogTarget) {
            outputGain_ -= rampStep_;
            if (outputGain_ < ogTarget) outputGain_ = ogTarget;
        }
        if (outputGain_ <= 0.0f && !playing) {
            break; // settled paused state: silence (stem ramps freeze here)
        }

        // Per-sample rate smoothing: the effective rate glides to the target
        // instead of stepping once per block (no zipper on big fader moves).
        rateSmoothed_ += (rateTarget - rateSmoothed_) * rateCoeff_;

        // Stem gain ramps, once per frame (SPEC §4.4).
        for (int s = 0; s < kStemCount; ++s) {
            const float target = stemTarget_[s].load(std::memory_order_relaxed) ? 1.0f : 0.0f;
            float g = stemGain_[s];
            if (g < target) {
                g += rampStep_;
                if (g > target) g = target;
            } else if (g > target) {
                g -= rampStep_;
                if (g < target) g = target;
            }
            stemGain_[s] = g;
        }

        float left = 0.0f;
        float right = 0.0f;
        if (xfadeRemaining_ > 0) {
            // ~2 ms declick crossfade: old position (still advancing) -> new.
            float ol = 0.0f, orr = 0.0f, nl = 0.0f, nr = 0.0f;
            mix(xfadeOldPos_, ol, orr);
            mix(playhead_, nl, nr);
            const float t = 1.0f - static_cast<float>(xfadeRemaining_) /
                                       static_cast<float>(declickFrames_);
            left = ol + (nl - ol) * t;
            right = orr + (nr - orr) * t;
            xfadeOldPos_ += rateSmoothed_;
            --xfadeRemaining_;
        } else {
            if (loopOn && playing && playhead_ >= loopOutF) {
                wrapLoop(); // activation/seek landed past the loop end
            }
            if (playhead_ >= static_cast<double>(d->frames)) {
                playhead_ = static_cast<double>(d->frames);
                break; // end of track: stop (SPEC §4.2)
            }
            if (playhead_ < 0.0) playhead_ = 0.0; // clamp at start when reversing
            // Paused: still mix at the frozen position so the transport fade
            // ramps the held sample instead of cutting to silence.
            mix(playhead_, left, right);
            if (playing) {
                playhead_ += rateSmoothed_;
                if (loopOn && playhead_ >= loopOutF) {
                    wrapLoop(); // exact crossing sample (SPEC §4.5)
                }
            }
        }

        const float sl = left * outputGain_;
        const float sr = right * outputGain_;
        out[i * 2] = sl;
        out[i * 2 + 1] = sr;
        const float a = std::fabs(sl) > std::fabs(sr) ? std::fabs(sl) : std::fabs(sr);
        if (a > peak) peak = a;
    }
    if (i < frameCount) {
        zero(out + i * 2, (frameCount - i) * 2);
    }
    if (playhead_ >= static_cast<double>(d->frames)) {
        playing_.store(false, std::memory_order_release);
    }
    displayPos_.store(playhead_, std::memory_order_relaxed);
    peak_.store(peak, std::memory_order_relaxed);
}

} // namespace tng
