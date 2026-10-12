// M9 BPM auto-detection (SPEC §5/§9): detectBpm() via SoundTouch
// BPMDetect, the loader contract (flat files auto-detect, a sidecar "bpm"
// wins over detection and survives cue edits), and silence/too-short
// guards. Fixtures: synthesized kick trains (build dir, removed after)
// plus one real-music case - examples/magnat.zip, 128 BPM per its own
// meta.json - which is the precise acceptance check (a perfectly
// periodic kick train only pins the tempo up to a small-integer ratio:
// its autocorrelation ties at every multiple of the beat lag).

#include "doctest.h"

#include "bpm_detect.h"
#include "testutil.h"
#include "track_loader.h"
#include "wav_fixture.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct TempBpm {
    std::string base; // full file name (extension included)
    explicit TempBpm(std::string b) : base(std::move(b)) {}
    ~TempBpm() {
        std::error_code ec;
        std::filesystem::remove(base, ec);
        std::filesystem::remove(base + ".cues.json", ec);
    }
};

// Decaying 80 Hz thump per beat - the bass-drum pattern BPMDetect targets
// (it works below ~250 Hz after decimating to ~1 kHz). Interleaved stereo.
std::vector<float> kickStereo(uint32_t fs, double seconds, double bpm) {
    const int64_t frames = static_cast<int64_t>(seconds * fs);
    const int64_t period = static_cast<int64_t>(fs * 60.0 / bpm + 0.5);
    std::vector<float> x(static_cast<size_t>(frames) * 2, 0.0f);
    for (int64_t i = 0; i + 512 <= frames; i += period) {
        for (int64_t k = 0; k < 512; ++k) {
            const float v = static_cast<float>(
                0.9 * std::sin(2.0 * tutil::kPi * 80.0 * static_cast<double>(k) / fs) *
                (1.0 - static_cast<double>(k) / 512.0));
            x[static_cast<size_t>(i + k) * 2] += v;
            x[static_cast<size_t>(i + k) * 2 + 1] += v;
        }
    }
    return x;
}

// The same kicks as interleaved s16 for the WAV fixtures.
std::vector<int16_t> kickPcm(uint32_t fs, double seconds, double bpm) {
    const std::vector<float> x = kickStereo(fs, seconds, bpm);
    std::vector<int16_t> pcm(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        pcm[i] = static_cast<int16_t>(x[i] * 32767.0f);
    }
    return pcm;
}

// True when `got` equals `want` or a small-integer multiple/division of it
// (within 1 BPM): the documented octave/triplet ambiguity of envelope
// autocorrelation on perfectly periodic material.
bool withinRatio(float got, double want) {
    if (got <= 0.0f) return false;
    for (double r : {1.0 / 3.0, 1.0 / 2.0, 1.0, 2.0, 3.0}) {
        if (std::fabs(got - static_cast<float>(want * r)) <= 1.0f) return true;
    }
    return false;
}

} // namespace

TEST_CASE("bpm: synthetic kick trains land on the tempo or a small-integer ratio") {
    const uint32_t fs = 44100;
    for (double bpm : {80.0, 128.0, 174.0}) {
        const std::vector<float> x = kickStereo(fs, 25.0, bpm);
        const float got = tng::detectBpm(x.data(), fs * 25, 2, fs);
        CHECK_MESSAGE(withinRatio(got, bpm), "want ", bpm, " got ", got);
    }
}

TEST_CASE("bpm: real music - magnat stem measures 128 BPM (SPEC 9)") {
    // The precise acceptance: real material disambiguates the ratio ties
    // that pure kick trains leave. Ground truth comes from the track's own
    // meta.json. Probes both cwds (ctest = build/, manual = repo root) and
    // skips cleanly when examples/ is absent.
    std::filesystem::path zip;
    for (const char* cand : {"../examples/magnat.zip", "examples/magnat.zip"}) {
        if (std::filesystem::exists(cand)) {
            zip = cand;
            break;
        }
    }
    if (zip.empty()) {
        MESSAGE("examples/magnat.zip not found - skipped");
        return;
    }
    tng::LoadResult res = tng::loadTrack(zip);
    REQUIRE_MESSAGE(res.ok(), res.error.c_str());
    REQUIRE(res.data->bpm > 0.0f); // meta.json's 128
    const tng::Stem& st = res.data->stems[0];
    REQUIRE(st.frames > 0);
    const float got = tng::detectBpm(st.data.data(), st.frames, 2, res.data->sampleRate);
    CHECK_MESSAGE(std::fabs(got - res.data->bpm) <= 1.0f, "meta says ",
                  res.data->bpm, ", detected ", got);
}

TEST_CASE("bpm: silence and too-short audio detect nothing") {
    const uint32_t fs = 44100;
    const std::vector<float> silence(static_cast<size_t>(fs) * 2, 0.0f);
    CHECK(tng::detectBpm(silence.data(), fs, 2, fs) == 0.0f);

    const std::vector<float> halfSec = kickStereo(fs, 0.5, 128.0);
    CHECK(tng::detectBpm(halfSec.data(), static_cast<int64_t>(0.5 * fs), 2, fs) ==
          0.0f); // under the one-second floor
}

TEST_CASE("bpm: loader auto-detects a flat kick file and labels it (SPEC 5/9)") {
    TempBpm f("bpm_flat_test.wav");
    tutil::writeWavStereo(f.base, 44100, kickPcm(44100, 25.0, 128.0));

    tng::LoadResult res = tng::loadTrack(f.base);
    REQUIRE_MESSAGE(res.ok(), res.error.c_str());
    CHECK(res.data->bpmDetected);
    CHECK(std::fabs(res.data->bpm - 128.0f) <= 1.0f); // 128 is tie-free
}

TEST_CASE("bpm: a sidecar bpm wins over detection and survives cue edits") {
    TempBpm f("bpm_sidecar_test.wav");
    tutil::writeWavStereo(f.base, 44100, kickPcm(44100, 5.0, 128.0));
    {
        std::ofstream sc(f.base + ".cues.json", std::ios::binary | std::ios::trunc);
        sc << R"({"cues": [{"name": "A", "position_ms": 0}], "bpm": 99.0})";
    }

    tng::LoadResult res = tng::loadTrack(f.base);
    REQUIRE_MESSAGE(res.ok(), res.error.c_str());
    CHECK(res.data->bpm == doctest::Approx(99.0f)); // sidecar, not detected
    CHECK_FALSE(res.data->bpmDetected);             // known, not analyzed now

    // A cue edit must not wipe the "bpm" key (saveCues round-trips extras).
    const std::vector<tng::Cue> edited = {{"A", 10.0}, {"B", 20.0}};
    std::string err;
    REQUIRE_MESSAGE(tng::saveCues(f.base, edited, &err), err.c_str());

    tng::LoadResult again = tng::loadTrack(f.base);
    REQUIRE_MESSAGE(again.ok(), again.error.c_str());
    REQUIRE(again.data->cues.size() == 2);
    CHECK(again.data->bpm == doctest::Approx(99.0f));
    CHECK_FALSE(again.data->bpmDetected);
}
