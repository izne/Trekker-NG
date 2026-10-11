#include "doctest.h"

#include "recorder.h"
#include "testutil.h"

#include "miniaudio.h" // ma_decoder for the file round-trips (impl: engine lib)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// M7 Recorder acceptance (SPEC 9.7). Files land in the test binary's working
// directory (the build dir - inside the project) and are removed again.
// 8000 Hz is not an MPEG-1 rate, so those takes are WAV no matter whether
// libmp3lame is installed - the assertions stay deterministic either way.
// ---------------------------------------------------------------------------

namespace {

// Test files: base path without extension; Recorder appends .wav / .mp3.
struct TempFile {
    std::string base;
    explicit TempFile(const std::string& name) : base(name) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(base + ".wav", ec);
        std::filesystem::remove(base + ".mp3", ec);
    }
};

// Deterministic per-index ramp in [-0.45, 0.45] - same expression for the
// pushed input and the expected output, so s16 quantization is the only error.
float rampAt(uint64_t i) {
    return static_cast<float>(static_cast<double>((i * 97) % 811) / 811.0 * 0.9 - 0.45);
}

std::vector<float> sineTrack(uint32_t fs, uint32_t frames, double freq, float amp) {
    std::vector<float> x(static_cast<size_t>(frames) * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        const float v =
            amp * static_cast<float>(std::sin(2.0 * tutil::kPi * freq * i / fs));
        x[static_cast<size_t>(i) * 2] = v;
        x[static_cast<size_t>(i) * 2 + 1] = v;
    }
    return x;
}

// Decode a finished file to interleaved float32. Returns {} when the file
// cannot be opened. The explicit f32 config matters: with a null config
// ma_decoder keeps the file's native format (s16 here).
std::vector<float> decodeFile(const std::string& path, uint32_t* fsOut) {
    std::vector<float> out;
    ma_decoder dec;
    const ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 0); // 0 Hz = file rate
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS) return out;
    if (fsOut != nullptr) *fsOut = dec.outputSampleRate;
    float buf[2048];
    for (;;) {
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&dec, buf, 1024, &got) != MA_SUCCESS || got == 0) break;
        out.insert(out.end(), buf, buf + got * dec.outputChannels);
    }
    ma_decoder_uninit(&dec);
    return out;
}

} // namespace

TEST_CASE("recorder: WAV round-trip is exact and the ring wraps (SPEC 9.7)") {
    TempFile f("recorder_wav_test");
    tng::Recorder rec;
    std::string err;
    // 8000 Hz -> not MPEG-1 -> WAV even with the lame DLL present.
    REQUIRE_MESSAGE(rec.start(f.base, 8000, &err), err.c_str());
    CHECK_FALSE(rec.usingMp3());
    CHECK(rec.filePath() == f.base + ".wav");

    // Capacity is 2 s = 16000 frames. Batch 1 (15000) fits unconditionally;
    // after the settle the writer has drained at least a few frames, so the
    // final 1001-frame push is accepted as well AND crosses the capacity
    // boundary (15000 + 1001 > 16000) - both ring memcpy branches run.
    const uint32_t total = 16001;
    std::vector<float> input(static_cast<size_t>(total) * 2);
    for (uint32_t i = 0; i < total; ++i) {
        input[static_cast<size_t>(i) * 2] = rampAt(i);
        input[static_cast<size_t>(i) * 2 + 1] = rampAt(i);
    }
    for (uint32_t off = 0; off < 15000; off += 3000) {
        rec.push(input.data() + static_cast<size_t>(off) * 2, 3000);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // writer settles
    rec.push(input.data() + static_cast<size_t>(15000) * 2, 1001);
    rec.stop();

    CHECK(rec.pushedFrames() == total);
    CHECK(rec.droppedFrames() == 0);
    CHECK_FALSE(rec.failed());

    uint32_t fs = 0;
    const std::vector<float> dec = decodeFile(rec.filePath(), &fs);
    REQUIRE(dec.size() == static_cast<size_t>(total) * 2); // WAV count is exact
    CHECK(fs == 8000);
    for (uint32_t i = 0; i < total; ++i) {
        const float got = dec[static_cast<size_t>(i) * 2];
        const float want = rampAt(i);
        if (std::fabs(got - want) > 2.0e-4f) {
            FAIL("frame ", i, ": got ", got, " want ", want); // s16 quantization only
            break;
        }
    }
}

TEST_CASE("recorder: an over-capacity block is dropped whole and counted (SPEC 9.7)") {
    TempFile f("recorder_drop_test");
    tng::Recorder rec;
    std::string err;
    REQUIRE_MESSAGE(rec.start(f.base, 8000, &err), err.c_str());
    // Fresh take: ring empty, so one 40000-frame block (> 16000 capacity)
    // must be rejected as a whole - a partial splice would corrupt the mix.
    std::vector<float> big(40000 * 2, 0.25f);
    rec.push(big.data(), 40000);
    CHECK(rec.pushedFrames() == 40000);
    CHECK(rec.droppedFrames() == 40000);
    rec.stop();
    CHECK_FALSE(rec.failed()); // drops are policy, not an error
    CHECK(decodeFile(rec.filePath(), nullptr).empty());
}

TEST_CASE("recorder: push allocates nothing while armed (SPEC 4.7/9.7)") {
    TempFile f("recorder_alloc_test");
    tng::Recorder rec;
    std::string err;
    REQUIRE_MESSAGE(rec.start(f.base, 8000, &err), err.c_str());
    std::vector<float> block(256 * 2, 0.1f); // allocate before the scope
    {
        rtcheck::Scope scope;
        for (int i = 0; i < 40; ++i) rec.push(block.data(), 256);
        CHECK(scope.count() == 0);
    }
    rec.stop(); // join/flush may allocate - off the audio path by design
}

TEST_CASE("recorder: MP3 round-trip when libmp3lame is available (SPEC 9.7)") {
    if (!tng::Recorder::lameAvailable()) {
        MESSAGE("libmp3lame-0.dll not found - MP3 round-trip skipped (WAV fallback)");
        return;
    }
    TempFile f("recorder_mp3_test");
    tng::Recorder rec;
    std::string err;
    const uint32_t fs = 44100;
    const uint32_t frames = fs; // 1 s, well under the 2 s ring -> no drops
    REQUIRE_MESSAGE(rec.start(f.base, fs, &err), err.c_str());
    REQUIRE(rec.usingMp3());
    CHECK(rec.filePath() == f.base + ".mp3");

    const std::vector<float> input = sineTrack(fs, frames, 440.0, 0.5f);
    rec.push(input.data(), frames);
    rec.stop();
    CHECK(rec.droppedFrames() == 0);
    CHECK_FALSE(rec.failed());

    uint32_t outFs = 0;
    const std::vector<float> dec = decodeFile(rec.filePath(), &outFs);
    REQUIRE(!dec.empty());
    CHECK(outFs == fs);
    // LAME adds the encoder delay (576) plus frame padding; no Xing tag
    // means the decoded stream starts at the first raw frame.
    const size_t decFrames = dec.size() / 2;
    CHECK(decFrames >= frames);
    CHECK(decFrames <= frames + 4000);

    // The content must still be the sine: peak + frequency through the middle.
    float peak = 0.0f;
    for (size_t i = 0; i < dec.size(); i += 2) peak = std::max(peak, std::fabs(dec[i]));
    CHECK(peak > 0.3f);
    const size_t mid = decFrames / 2;
    const size_t a = mid > 8192 ? mid - 8192 : 1;
    const size_t b = std::min(mid + 8192, decFrames - 1);
    const double freq = tutil::measureFreq(dec, static_cast<int64_t>(a),
                                           static_cast<int64_t>(b), fs, 0);
    CHECK(std::fabs(freq - 440.0) < 5.0);
}

TEST_CASE("recorder: lifecycle guards (double start, stop idempotence)") {
    TempFile f("recorder_life_test");
    tng::Recorder rec;
    std::string err;
    REQUIRE_MESSAGE(rec.start(f.base, 8000, &err), err.c_str());
    CHECK_FALSE(rec.start(f.base + "_second", 8000, &err)); // already recording
    CHECK(!err.empty());
    std::vector<float> block(256 * 2, 0.0f);
    rec.push(block.data(), 256);
    const uint64_t pushed = rec.pushedFrames();
    rec.stop();
    rec.stop(); // idempotent
    rec.push(block.data(), 256); // disarmed: ignored, nothing counted
    CHECK(rec.pushedFrames() == pushed);
    CHECK(rec.recording() == false);
}
