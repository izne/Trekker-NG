// Flat / single-file tracks (SPEC 5, M8): a plain audio file loads through
// loadTrack() as a one-stem flat DeckData; hot cues live in a
// "<file>.cues.json" sidecar; non-audio files fail with a decode error.
// Fixtures are tiny WAVs written into the test binary's working directory
// (the build dir - inside the project) and removed again.

#include "doctest.h"

#include "testutil.h"
#include "track_loader.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct TempFlat {
    std::string base; // full file name (extension included)
    explicit TempFlat(std::string b) : base(std::move(b)) {}
    ~TempFlat() {
        std::error_code ec;
        std::filesystem::remove(base, ec);
        std::filesystem::remove(base + ".cues.json", ec);
    }
};

void writeU16(std::ostream& o, uint16_t v) {
    o.put(static_cast<char>(v & 0xFF));
    o.put(static_cast<char>((v >> 8) & 0xFF));
}

void writeU32(std::ostream& o, uint32_t v) {
    writeU16(o, static_cast<uint16_t>(v & 0xFFFF));
    writeU16(o, static_cast<uint16_t>(v >> 16));
}

// Minimal 16-bit PCM stereo WAV (PCM + interleaved s16 is what
// decodeStem's ma_decoder reads back exactly).
void writeWavStereo(const std::string& path, uint32_t fs,
                    const std::vector<int16_t>& pcm) {
    std::ofstream f(path, std::ios::binary);
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4);
    writeU32(f, 36 + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    writeU32(f, 16);
    writeU16(f, 1);                     // PCM
    writeU16(f, 2);                     // stereo
    writeU32(f, fs);
    writeU32(f, fs * 2 * 2);            // byte rate
    writeU16(f, 2 * 2);                 // block align
    writeU16(f, 16);                    // bits per sample
    f.write("data", 4);
    writeU32(f, dataBytes);
    for (int16_t s : pcm) writeU16(f, static_cast<uint16_t>(s));
}

// 440 Hz sine at amplitude 0.5, interleaved L=R.
std::vector<int16_t> sinePcm(uint32_t fs, uint32_t frames) {
    std::vector<int16_t> pcm(static_cast<size_t>(frames) * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        const double v = 0.5 * std::sin(2.0 * tutil::kPi * 440.0 * i / fs);
        const int16_t s = static_cast<int16_t>(v * 32767.0);
        pcm[static_cast<size_t>(i) * 2] = s;
        pcm[static_cast<size_t>(i) * 2 + 1] = s;
    }
    return pcm;
}

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("flat: a plain audio file loads as a one-stem flat track (SPEC 5)") {
    TempFlat f("flat_track_test.wav");
    const uint32_t fs = 44100;
    const uint32_t frames = 1000;
    writeWavStereo(f.base, fs, sinePcm(fs, frames));

    tng::LoadResult res = tng::loadTrack(f.base);
    REQUIRE_MESSAGE(res.ok(), res.error.c_str());
    const tng::DeckData& d = *res.data;

    CHECK(d.flat); // the UI keys the flat layout (empty lane box) off this
    CHECK(d.title == "flat_track_test");
    CHECK(d.artist.empty());
    CHECK(d.bpm == doctest::Approx(0.0f)); // unknown without meta.json
    CHECK(d.sampleRate == fs);
    CHECK(d.frames == static_cast<int64_t>(frames));
    CHECK(d.cues.empty());

    // Stem 0 carries the file; 1..3 stay the documented "missing = silent".
    CHECK(d.stems[0].frames == static_cast<int64_t>(frames));
    CHECK(d.stems[0].color == 0xFFDDDDDDu); // neutral gray (no meta color)
    for (int s = 1; s < 4; ++s) {
        CHECK(d.stems[static_cast<size_t>(s)].frames == 0);
        CHECK(d.stems[static_cast<size_t>(s)].data.empty());
    }

    // The decoded content is the sine (s16 quantization only).
    for (uint32_t i : {0u, 1u, 250u, 999u}) {
        const float want = static_cast<float>(0.5 * std::sin(2.0 * tutil::kPi * 440.0 *
                                                            i / fs));
        const float got = d.stems[0].data[static_cast<size_t>(i) * 2];
        CHECK(std::fabs(got - want) < 2.0e-4f);
    }
}

TEST_CASE("flat: cues round-trip through the .cues.json sidecar (SPEC 5/9.8)") {
    TempFlat f("flat_cues_test.wav");
    writeWavStereo(f.base, 44100, sinePcm(44100, 200));

    {
        std::ofstream sc(f.base + ".cues.json", std::ios::binary | std::ios::trunc);
        sc << R"({"cues":[
            {"name": "Intro", "position_ms": 100},
            {"name": "Drop", "position_ms": 2500}
        ]})";
    }
    tng::LoadResult res = tng::loadTrack(f.base);
    REQUIRE_MESSAGE(res.ok(), res.error.c_str());
    REQUIRE(res.data->cues.size() == 2);
    CHECK(res.data->cues[0].name == "Intro");
    CHECK(res.data->cues[0].positionMs == doctest::Approx(100.0));
    CHECK(res.data->cues[1].name == "Drop");
    CHECK(res.data->cues[1].positionMs == doctest::Approx(2500.0));

    // Edit + save: the sidecar updates, the audio file stays untouched.
    const std::vector<uint8_t> before = readBytes(f.base);
    const std::vector<tng::Cue> edited = {{"A", 10.0}, {"B", 20.0}, {"C", 30.0}};
    std::string err;
    REQUIRE_MESSAGE(tng::saveCues(f.base, edited, &err), err.c_str());
    CHECK(readBytes(f.base) == before);

    tng::LoadResult again = tng::loadTrack(f.base);
    REQUIRE_MESSAGE(again.ok(), again.error.c_str());
    REQUIRE(again.data->cues.size() == 3);
    CHECK(again.data->cues[2].name == "C");
    CHECK(again.data->cues[2].positionMs == doctest::Approx(30.0));

    // A corrupt sidecar is ignored (same rule as zip tracks): no cues.
    {
        std::ofstream sc(f.base + ".cues.json", std::ios::binary | std::ios::trunc);
        sc << R"({"cues": "not-an-array"})";
    }
    tng::LoadResult corrupt = tng::loadTrack(f.base);
    REQUIRE_MESSAGE(corrupt.ok(), corrupt.error.c_str());
    CHECK(corrupt.data->cues.empty());
}

TEST_CASE("flat: a non-audio file fails with a decode error (SPEC 5)") {
    TempFlat f("flat_garbage_test.txt");
    {
        std::ofstream out(f.base, std::ios::binary | std::ios::trunc);
        out << "this is not audio";
    }
    tng::LoadResult res = tng::loadTrack(f.base);
    REQUIRE_FALSE(res.ok());
    CHECK(res.error.find("decode failed") != std::string::npos);
}
