#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace tng {

// M10 (SPEC §4.8): preallocated lock-free SPSC stereo ring bridging the two
// output devices. Producer = master audio callback (writes the blended
// phones signal after Mixer::process), consumer = cue device callback
// (drains it). Both sides are realtime (SPEC §4.7): no allocation, no
// locks, no IO - only relaxed/acquire-release atomics and copies; the
// storage is sized once in resize() while the devices are stopped.
//
// The two sound cards run on independent clocks, so steady state drifts by
// a few ms per hour: the ring absorbs it as slowly shifting fill levels.
// Hard failures are bounded on both sides - on overrun the producer drops
// the whole incoming block (counted), on underrun the consumer zero-fills
// the rest of its output (counted). dropped()/underrun() are relaxed
// atomics so the UI or a test can poll them from any thread.
class CueRing {
public:
    // Setup-time only (devices stopped). capacityFrames == 0 frees the
    // buffer; the ring then drops/zero-fills everything until resized.
    void resize(uint32_t capacityFrames) noexcept;
    void clear() noexcept; // reset indices + counters, keep the storage

    // Producer. Writes `frames` interleaved stereo frames, or drops them
    // all when fewer than `frames` are free. Returns written frame count.
    uint32_t push(const float* interleaved, uint32_t frames) noexcept;

    // Consumer. Reads up to `frames`; a short ring zero-fills the rest of
    // the output (an underrun) - the cue callback never branches on this.
    void pop(float* interleaved, uint32_t frames) noexcept;

    uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }
    uint64_t underrun() const noexcept {
        return underrun_.load(std::memory_order_relaxed);
    }
    uint32_t capacityFrames() const noexcept { return capacity_; }

private:
    std::vector<float> buf_;      // capacity_ frames * 2 floats
    uint32_t capacity_ = 0;       // in frames (power of two not required)
    // Per-thread caches of the *other* side's position - each field is
    // touched by exactly one thread (producer reads cachedRead_, consumer
    // reads cachedWrite_), the atomics below carry the cross-thread
    // values, so there is no data race.
    uint64_t cachedRead_ = 0;   // producer's snapshot of readIdx_
    uint64_t cachedWrite_ = 0;  // consumer's snapshot of writeIdx_
    std::atomic<uint64_t> readIdx_{0};  // consumer advances: frames consumed
    std::atomic<uint64_t> writeIdx_{0}; // producer advances: frames produced
    std::atomic<uint64_t> dropped_{0};  // producer-side, dropped frames
    std::atomic<uint64_t> underrun_{0}; // consumer-side, silent frames
};

} // namespace tng
