#include "doctest.h"
#include "interpolate.h"
#include "testutil.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

std::vector<float> makeRamp() {
    std::vector<float> buf(64);
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<float>(i) * 0.37f - 5.0f;
    }
    return buf;
}

} // namespace

TEST_CASE("interpolator reproduces samples exactly at integer positions (SPEC 9.5)") {
    const std::vector<float> buf = makeRamp();
    for (const sde::InterpMode mode : {sde::InterpMode::Linear, sde::InterpMode::Hermite}) {
        for (int64_t i = 0; i < static_cast<int64_t>(buf.size()); ++i) {
            const float got = sde::readFrame(buf.data(), static_cast<int64_t>(buf.size()), 1, 0,
                                             static_cast<double>(i), mode);
            CHECK(got == buf[static_cast<size_t>(i)]); // bit-identical, no error
        }
    }
}

TEST_CASE("linear interpolation is exact at midpoints") {
    std::vector<float> buf = {0.0f, 10.0f};
    const float got =
        sde::readFrame(buf.data(), 2, 1, 0, 0.5, sde::InterpMode::Linear);
    CHECK(got == 5.0f);
}

TEST_CASE("interleaved stereo: each channel reads its own lane") {
    std::vector<float> buf(16);
    for (size_t i = 0; i < buf.size(); i += 2) {
        buf[i] = static_cast<float>(i);       // ch0: 0,2,4,...
        buf[i + 1] = static_cast<float>(i) + 100.0f; // ch1: 101,103,...
    }
    CHECK(sde::readFrame(buf.data(), 8, 2, 0, 3.0, sde::InterpMode::Linear) == 6.0f);
    CHECK(sde::readFrame(buf.data(), 8, 2, 1, 3.0, sde::InterpMode::Linear) == 106.0f);
}

TEST_CASE("positions outside the buffer are silence (no reads OOB)") {
    const std::vector<float> buf = makeRamp();
    for (const double pos : {-0.5, -1.0, 64.0, 64.7, 1000.0}) {
        const float got = sde::readFrame(buf.data(), static_cast<int64_t>(buf.size()), 1, 0,
                                         pos, sde::InterpMode::Hermite);
        CHECK(got == 0.0f);
    }
}

TEST_CASE("hermite clamps neighbour taps at the buffer edges") {
    std::vector<float> buf = makeRamp();
    // Near the left edge the y0 tap must clamp to frame 0 rather than read OOB.
    const float atEdge = sde::readFrame(buf.data(), 64, 1, 0, 0.25, sde::InterpMode::Hermite);
    const float clamped =
        sde::interpHermite(buf[0], buf[0], buf[1], buf[2], 0.25f);
    CHECK(atEdge == clamped);
    CHECK(std::isfinite(atEdge));
}

TEST_CASE("deck at rate 1.0: output equals input bit-exactly (SPEC 9.5)") {
    const uint32_t fs = 44100;
    auto track = tutil::makeSineTrack(fs, 2.0, {440.0});
    sde::Deck deck;
    deck.setTrack(std::move(track));
    deck.setPlaying(true);

    const int64_t n = fs / 2;
    const std::vector<float> out = tutil::renderFrames(deck, n);
    const sde::DeckData* d = deck.track();
    REQUIRE(d != nullptr);

    // After the 5 ms transport fade-in outputGain sits at exactly 1.0f and
    // every playhead position is an integer (rate 1.0) => the rendered
    // buffer must be a bit-identical copy of the stem, forever.
    const int64_t skip = 1000; // well past the 221-frame fade-in
    const size_t floats = static_cast<size_t>(n - skip) * 2;
    CHECK(std::memcmp(out.data() + skip * 2, d->stems[0].data.data() + skip * 2,
                      floats * sizeof(float)) == 0);
}
