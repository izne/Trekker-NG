#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace tng {

// Recorder (M7, SPEC 4.1 tap + 4.7 realtime rules): captures the master bus
// (post-EQ / crossfader / limiter - exactly what the soundcard plays) into a
// file while the device runs.
//
// Threading: push() runs on the audio thread and only does atomic flag/index
// work plus a ring copy - no allocation, no locks, no IO, no exceptions
// (SPEC 4.7). start()/stop()/queries run on the UI (or test) thread. The
// ring is allocated on the first start() and freed only in the destructor;
// stop() disarms the gate and waits out any in-flight push, and the Recorder
// outlives the device (same contract as Deck/Mixer), so the callback never
// touches freed memory.
//
// Format (SPEC 3 / 8): MP3 CBR 320 kbps through libmp3lame-0.dll loaded at
// runtime (LoadLibrary - no link-time dependency, the exe runs and records
// WAV without the DLL), with automatic WAV fallback (ma_encoder, s16) when
// the DLL is missing or the sample rate is not MPEG-1 (32/44.1/48 kHz).
// A writer thread drains the ring, converts f32 -> s16 (clamped like the
// console --render path) and encodes; when the ring is full (disk too slow)
// the incoming block is dropped whole and its frames are counted instead -
// droppedFrames()/failed() surface it to the UI (SPEC 8 M7).
class Recorder {
public:
    Recorder();
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // UI thread. `pathBase` is the full output path WITHOUT extension; the
    // encoder appends ".mp3" or ".wav" (see filePath()). Returns false with
    // an error message when already recording, sampleRate is 0, or the file
    // cannot be created. Arms the audio-thread gate last, so no frame is
    // captured before everything is ready.
    bool start(const std::string& pathBase, uint32_t sampleRate, std::string* error);

    // UI thread: disarm, drain, finalize the file, join the writer. No-op
    // when idle. filePath() keeps the finished path afterwards.
    void stop();

    bool recording() const noexcept { return recording_.load(std::memory_order_acquire); }
    bool usingMp3() const noexcept { return mp3_.load(std::memory_order_relaxed); }
    bool failed() const noexcept { return failed_.load(std::memory_order_relaxed); }
    uint64_t pushedFrames() const noexcept { return pushed_.load(std::memory_order_relaxed); }
    uint64_t droppedFrames() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    // Final path of the current/last take ("" before the first start()).
    const std::string& filePath() const { return path_; }

    // Audio thread: append one interleaved stereo block to the capture.
    void push(const float* interleaved, uint32_t frames) noexcept;

    // True when libmp3lame-0.dll loads (cached probe, first caller decides).
    static bool lameAvailable();

private:
    struct Impl; // encoder state (ma_encoder / LAME handle), cpp-only
    Impl* impl_;

    void writerLoop();   // thread body: ring -> s16 -> encoder -> file
    void closeEncoder(); // finalize/flush + close (no thread running)

    std::vector<float> ring_; // frames * 2; first start(), kept until ~Recorder
    size_t capacity_ = 0;     // ring capacity in frames (sampleRate * 2 s)

    std::atomic<size_t> readIdx_{0};  // writer cursor, monotonic total frames
    std::atomic<size_t> writeIdx_{0}; // producer cursor, monotonic total frames
    std::atomic<bool> armed_{false};  // audio-thread gate
    std::atomic<int> inFlight_{0};    // pushes between entry and gate check
    std::atomic<bool> stopReq_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> mp3_{false};
    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> pushed_{0};
    std::atomic<uint64_t> dropped_{0};

    uint32_t rate_ = 0;
    std::string path_;      // final path while recording (UI thread only)
    std::thread writer_;    // drains ring -> encoder (UI thread joins it)
};

} // namespace tng
