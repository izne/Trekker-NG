#include "recorder.h"

#include "miniaudio.h" // ma_encoder (WAV) - implementation lives in audio_device.cpp

#include <windows.h> // LoadLibraryW/GetProcAddress for libmp3lame-0.dll

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

// Opaque LAME context. lame.h declares the same struct tag and callers only
// ever hold the pointer - the installed header is not a build dependency
// (SPEC 3: libmp3lame enters at runtime through the DLL).
struct lame_global_flags;

namespace {

// libmp3lame resolved at runtime (SPEC 3/8): no import lib, no link-time
// dependency - without the DLL the Recorder falls back to WAV and the exe
// still starts. The prototypes are the frozen LAME C API; MPEG_mode is a C
// enum so an int is ABI-identical for lame_set_mode.
struct LameApi {
    HMODULE mod = nullptr;
    lame_global_flags* (*initFn)() = nullptr;
    int (*setIn)(lame_global_flags*, int) = nullptr;
    int (*setCh)(lame_global_flags*, int) = nullptr;
    int (*setMode)(lame_global_flags*, int) = nullptr;
    int (*setQuality)(lame_global_flags*, int) = nullptr;
    int (*setBrate)(lame_global_flags*, int) = nullptr;
    int (*setVbrTag)(lame_global_flags*, int) = nullptr;
    int (*initParams)(lame_global_flags*) = nullptr;
    int (*encodeInter)(lame_global_flags*, short*, int, unsigned char*, int) = nullptr;
    int (*flushFn)(lame_global_flags*, unsigned char*, int) = nullptr;
    int (*closeFn)(lame_global_flags*) = nullptr;
    bool ok = false;
};

template <typename T>
T loadFn(HMODULE mod, const char* name) {
    // Via void*: a direct FARPROC -> unrelated-function-pointer cast trips
    // -Wcast-function-type; Windows x64 has one calling convention, so the
    // plain reinterprets below are safe.
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(mod, name)));
}

LameApi& lameApi() {
    static LameApi api = []() {
        LameApi a;
        a.mod = LoadLibraryW(L"libmp3lame-0.dll");
        if (a.mod == nullptr) return a;
        a.initFn = loadFn<decltype(a.initFn)>(a.mod, "lame_init");
        a.setIn = loadFn<decltype(a.setIn)>(a.mod, "lame_set_in_samplerate");
        a.setCh = loadFn<decltype(a.setCh)>(a.mod, "lame_set_num_channels");
        a.setMode = loadFn<decltype(a.setMode)>(a.mod, "lame_set_mode");
        a.setQuality = loadFn<decltype(a.setQuality)>(a.mod, "lame_set_quality");
        a.setBrate = loadFn<decltype(a.setBrate)>(a.mod, "lame_set_brate");
        a.setVbrTag = loadFn<decltype(a.setVbrTag)>(a.mod, "lame_set_bWriteVbrTag");
        a.initParams = loadFn<decltype(a.initParams)>(a.mod, "lame_init_params");
        a.encodeInter =
            loadFn<decltype(a.encodeInter)>(a.mod, "lame_encode_buffer_interleaved");
        a.flushFn = loadFn<decltype(a.flushFn)>(a.mod, "lame_encode_flush");
        a.closeFn = loadFn<decltype(a.closeFn)>(a.mod, "lame_close");
        a.ok = a.initFn != nullptr && a.setIn != nullptr && a.setCh != nullptr &&
               a.setMode != nullptr && a.setQuality != nullptr && a.setBrate != nullptr &&
               a.setVbrTag != nullptr && a.initParams != nullptr && a.encodeInter != nullptr &&
               a.flushFn != nullptr && a.closeFn != nullptr;
        if (!a.ok) {
            FreeLibrary(a.mod);
            a.mod = nullptr;
        }
        return a;
    }();
    return api;
}

// SPEC 8 (M7): MP3 only at MPEG-1 rates; everything else records WAV.
bool isMpeg1Rate(uint32_t rate) {
    return rate == 32000 || rate == 44100 || rate == 48000;
}

// Recorder paths are UTF-8 strings (built from SDL_GetBasePath()); on
// Windows the wide entry points need the real UTF-16 form - a narrow
// fopen() of UTF-8 bytes would silently fail in non-ASCII install dirs.
FILE* openOutFile(const std::string& utf8Path) {
#ifdef _WIN32
    const std::wstring wide = std::filesystem::u8path(utf8Path).wstring();
    return _wfopen(wide.c_str(), L"wb");
#else
    return std::fopen(utf8Path.c_str(), "wb");
#endif
}

} // namespace

namespace tng {

struct Recorder::Impl {
    FILE* file = nullptr;             // mp3 output (writer thread owns it)
    lame_global_flags* gfp = nullptr; // mp3 encoder, null = WAV
    ma_encoder enc{};                 // WAV encoder
    bool wav = false;
};

Recorder::Recorder() : impl_(new Impl) {}

Recorder::~Recorder() {
    stop();
    delete impl_;
    impl_ = nullptr;
}

bool Recorder::lameAvailable() { return lameApi().ok; }

// Finalize/flush everything start() opened. Runs on the writer thread after
// the loop, or on the UI thread only when no thread was ever spawned.
void Recorder::closeEncoder() {
    if (impl_->gfp != nullptr) {
        lameApi().closeFn(impl_->gfp);
        impl_->gfp = nullptr;
    }
    if (impl_->wav) {
        ma_encoder_uninit(&impl_->enc);
        impl_->wav = false;
    }
    if (impl_->file != nullptr) {
        std::fclose(impl_->file);
        impl_->file = nullptr;
    }
}

bool Recorder::start(const std::string& pathBase, uint32_t sampleRate, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error != nullptr) *error = msg;
        return false;
    };
    if (recording_.load(std::memory_order_acquire)) return fail("already recording");
    if (sampleRate == 0) return fail("sample rate is 0");

    // Format pick: MP3 when the DLL is there and the rate is MPEG-1,
    // otherwise WAV (SPEC 8 M7). Encoder state must be clean for either.
    mp3_.store(false, std::memory_order_relaxed);
    path_.clear();
    const bool wantMp3 = lameAvailable() && isMpeg1Rate(sampleRate);
    if (wantMp3) {
        LameApi& api = lameApi();
        lame_global_flags* gfp = api.initFn();
        bool opened = false;
        if (gfp != nullptr) {
            api.setIn(gfp, static_cast<int>(sampleRate));
            api.setCh(gfp, 2);
            api.setMode(gfp, 1);    // JOINT_STEREO (lame.h MPEG_mode value)
            api.setQuality(gfp, 2); // LAME default: good quality, fast
            api.setBrate(gfp, 320); // CBR 320 kbps (SPEC 8 M7)
            api.setVbrTag(gfp, 0);  // no Xing tag: plain CBR frames from byte 0
            if (api.initParams(gfp) >= 0) {
                path_ = pathBase + ".mp3";
                impl_->file = openOutFile(path_);
                if (impl_->file != nullptr) {
                    impl_->gfp = gfp;
                    opened = true;
                }
            }
            if (!opened) api.closeFn(gfp);
        }
        if (opened) mp3_.store(true, std::memory_order_relaxed);
    }
    if (!mp3_.load(std::memory_order_relaxed)) {
        path_ = pathBase + ".wav";
        const std::wstring wide = std::filesystem::u8path(path_).wstring();
        ma_encoder_config cfg =
            ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 2, sampleRate);
        if (ma_encoder_init_file_w(wide.c_str(), &cfg, &impl_->enc) != MA_SUCCESS) {
            path_.clear();
            return fail("cannot create " + pathBase);
        }
        impl_->wav = true;
    }

    // Ring: 2 s of stereo float32. Allocated once on first start and never
    // touched while a take is armed (start() is rejected when recording).
    const size_t need = static_cast<size_t>(sampleRate) * 2;
    if (capacity_ < need || ring_.size() < need * 2) {
        capacity_ = need;
        ring_.assign(need * 2, 0.0f);
    }
    readIdx_.store(0, std::memory_order_relaxed);
    writeIdx_.store(0, std::memory_order_relaxed);
    pushed_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    stopReq_.store(false, std::memory_order_relaxed);
    rate_ = sampleRate;
    recording_.store(true, std::memory_order_release);
    try {
        writer_ = std::thread(&Recorder::writerLoop, this);
    } catch (...) {
        recording_.store(false, std::memory_order_release);
        closeEncoder();
        path_.clear();
        return fail("cannot start the writer thread");
    }
    armed_.store(true, std::memory_order_release); // gate opens last
    return true;
}

void Recorder::stop() {
    if (!recording_.load(std::memory_order_acquire)) return;
    armed_.store(false, std::memory_order_release);
    // Wait out pushes that entered before the disarm (SPEC 4.7: the audio
    // thread never waits - only we do, on the UI thread).
    while (inFlight_.load(std::memory_order_acquire) != 0) std::this_thread::yield();
    stopReq_.store(true, std::memory_order_release);
    if (writer_.joinable()) writer_.join(); // drains, flushes, closes
    recording_.store(false, std::memory_order_release);
}

void Recorder::push(const float* interleaved, uint32_t frames) noexcept {
    if (frames == 0) return;
    // Counted before the gate check so stop() can never observe inFlight_
    // == 0 while a push that saw armed_ == true is still running.
    inFlight_.fetch_add(1, std::memory_order_acq_rel);
    if (!armed_.load(std::memory_order_acquire)) {
        inFlight_.fetch_sub(1, std::memory_order_release);
        return;
    }
    const size_t w = writeIdx_.load(std::memory_order_relaxed);
    const size_t r = readIdx_.load(std::memory_order_acquire);
    pushed_.fetch_add(frames, std::memory_order_relaxed);
    if (capacity_ == 0 || w - r + frames > capacity_) {
        // Disk too slow: drop the block whole and count it (SPEC 8 M7 -
        // the UI shows the warning); a partial write would splice the mix.
        dropped_.fetch_add(frames, std::memory_order_relaxed);
    } else {
        const size_t off = w % capacity_;
        size_t first = capacity_ - off;
        if (first > frames) first = frames;
        std::memcpy(ring_.data() + off * 2, interleaved, first * 2 * sizeof(float));
        if (first < frames) {
            std::memcpy(ring_.data(), interleaved + first * 2,
                        (frames - first) * 2 * sizeof(float));
        }
        writeIdx_.store(w + frames, std::memory_order_release);
    }
    inFlight_.fetch_sub(1, std::memory_order_release);
}

// Writer thread: drain ring -> s16 -> encoder. Polling every 2 ms keeps the
// audio thread completely out of any mutex/cv (SPEC 4.7); 2 ms of ring
// latency is inaudible (the file is not a live path).
void Recorder::writerLoop() {
    constexpr size_t kChunk = 4096; // frames per drain step
    std::vector<float> fin(kChunk * 2);
    std::vector<short> fout(kChunk * 2);
    // 1.25 * n + 7200 is LAME's documented worst-case output bound (lame.h).
    std::vector<unsigned char> mout(kChunk + kChunk / 4 + 7200);
    LameApi& api = lameApi();
    size_t consumed = 0;
    for (;;) {
        const size_t w = writeIdx_.load(std::memory_order_acquire);
        if (w > consumed) {
            size_t n = w - consumed;
            if (n > kChunk) n = kChunk;
            const size_t off = consumed % capacity_;
            size_t first = capacity_ - off;
            if (first > n) first = n;
            std::memcpy(fin.data(), ring_.data() + off * 2, first * 2 * sizeof(float));
            if (first < n) {
                std::memcpy(fin.data() + first * 2, ring_.data(),
                            (n - first) * 2 * sizeof(float));
            }
            for (size_t i = 0; i < n * 2; ++i) {
                float v = fin[i];
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                fout[i] = static_cast<short>(v * 32767.0f);
            }
            if (impl_->gfp != nullptr) {
                const int nb = api.encodeInter(impl_->gfp, fout.data(), static_cast<int>(n),
                                               mout.data(), static_cast<int>(mout.size()));
                if (nb < 0) {
                    failed_.store(true, std::memory_order_relaxed);
                } else if (nb > 0 && std::fwrite(mout.data(), 1, static_cast<size_t>(nb),
                                                 impl_->file) != static_cast<size_t>(nb)) {
                    failed_.store(true, std::memory_order_relaxed);
                }
            } else if (impl_->wav) {
                ma_uint64 got = 0;
                ma_encoder_write_pcm_frames(&impl_->enc, fout.data(), n, &got);
                if (got != n) failed_.store(true, std::memory_order_relaxed); // disk full
            }
            consumed += n;
            readIdx_.store(consumed, std::memory_order_release);
            continue;
        }
        if (stopReq_.load(std::memory_order_acquire)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Finalize on the writer thread: flush the encoder tail, then close.
    if (impl_->gfp != nullptr && impl_->file != nullptr) {
        std::vector<unsigned char> tail(7200 + kChunk);
        const int nb = api.flushFn(impl_->gfp, tail.data(), static_cast<int>(tail.size()));
        if (nb < 0) {
            failed_.store(true, std::memory_order_relaxed);
        } else if (nb > 0 && std::fwrite(tail.data(), 1, static_cast<size_t>(nb),
                                         impl_->file) != static_cast<size_t>(nb)) {
            failed_.store(true, std::memory_order_relaxed);
        }
    }
    closeEncoder();
}

} // namespace tng
