#include "cue_ring.h"

#include <algorithm>
#include <cstring>

namespace tng {

void CueRing::resize(uint32_t capacityFrames) noexcept {
    capacity_ = capacityFrames;
    buf_.assign(static_cast<size_t>(capacityFrames) * 2, 0.0f);
    clear();
}

void CueRing::clear() noexcept {
    cachedRead_ = 0;
    cachedWrite_ = 0;
    readIdx_.store(0, std::memory_order_relaxed);
    writeIdx_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    underrun_.store(0, std::memory_order_relaxed);
}

uint32_t CueRing::push(const float* interleaved, uint32_t frames) noexcept {
    if (frames == 0) return 0;
    if (capacity_ == 0 || interleaved == nullptr) {
        dropped_.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }
    cachedRead_ = readIdx_.load(std::memory_order_acquire);
    const uint64_t writePos = writeIdx_.load(std::memory_order_relaxed); // single writer
    const uint64_t used = writePos - cachedRead_;
    const uint64_t free = static_cast<uint64_t>(capacity_) - used;
    if (frames > free) {
        // Overrun: the consumer is gone or behind - drop the block whole.
        dropped_.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }
    const uint64_t start = writePos % capacity_;
    for (uint32_t i = 0; i < frames; ++i) {
        const uint64_t pos = (start + i) % capacity_;
        buf_[static_cast<size_t>(pos) * 2] = interleaved[i * 2];
        buf_[static_cast<size_t>(pos) * 2 + 1] = interleaved[i * 2 + 1];
    }
    writeIdx_.store(writePos + frames, std::memory_order_release);
    return frames;
}

void CueRing::pop(float* interleaved, uint32_t frames) noexcept {
    if (frames == 0 || interleaved == nullptr) return;
    uint64_t avail = 0;
    if (capacity_ > 0) {
        cachedWrite_ = writeIdx_.load(std::memory_order_acquire);
        const uint64_t readPos = readIdx_.load(std::memory_order_relaxed); // single writer
        avail = std::min<uint64_t>(cachedWrite_ - readPos, frames);
    }
    if (avail < frames) {
        std::memset(interleaved + avail * 2, 0,
                    static_cast<size_t>(frames - avail) * 2 * sizeof(float));
        underrun_.fetch_add(frames - avail, std::memory_order_relaxed);
    }
    if (avail > 0) {
        const uint64_t readPos = readIdx_.load(std::memory_order_relaxed);
        const uint64_t start = readPos % capacity_;
        for (uint64_t i = 0; i < avail; ++i) {
            const uint64_t pos = (start + i) % capacity_;
            interleaved[i * 2] = buf_[static_cast<size_t>(pos) * 2];
            interleaved[i * 2 + 1] = buf_[static_cast<size_t>(pos) * 2 + 1];
        }
        readIdx_.store(readPos + avail, std::memory_order_release);
    }
}

} // namespace tng
