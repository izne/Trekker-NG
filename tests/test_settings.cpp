// Settings tests (M5c): JSON round-trip, defaults on corrupt input, value
// clamping. The path-based load()/save() are thin IO wrappers (like
// saveCues) and stay out of the suite - the in-memory core is what matters.

#include "doctest.h"

#include "settings.h"

TEST_CASE("settings: defaults") {
    tng::Settings s;
    CHECK(s.deviceName.empty());
    CHECK(s.bufferFrames == 256);
    CHECK(s.pitchRangePct == 10.0f);
    CHECK(s.pitchReversed); // DJ-style up = slower (SPEC 4.3)
    CHECK(!s.bootMixMode);
    CHECK(s.xfCurve == 1); // constant power
    CHECK(s.masterGain == doctest::Approx(0.7f));
}

TEST_CASE("settings: json round-trip") {
    tng::Settings a;
    a.deviceName = "Speakers (USB Audio)";
    a.bufferFrames = 512;
    a.pitchRangePct = 16.0f;
    a.pitchReversed = false;
    a.bootMixMode = true;
    a.xfCurve = 2; // sharp cut
    a.masterGain = 0.85f;

    tng::Settings b;
    REQUIRE(b.fromJson(a.toJson()));
    CHECK(b.deviceName == a.deviceName);
    CHECK(b.bufferFrames == a.bufferFrames);
    CHECK(b.pitchRangePct == doctest::Approx(a.pitchRangePct));
    CHECK(b.pitchReversed == a.pitchReversed);
    CHECK(b.bootMixMode == a.bootMixMode);
    CHECK(b.xfCurve == a.xfCurve);
    CHECK(b.masterGain == doctest::Approx(a.masterGain));
}

TEST_CASE("settings: corrupt or empty text keeps defaults") {
    tng::Settings s;
    s.masterGain = 0.55f; // a non-default to prove "keeps current values"
    CHECK(!s.fromJson(""));
    CHECK(!s.fromJson("{ not json"));
    CHECK(!s.fromJson("[1,2,3]")); // not an object
    CHECK(s.masterGain == doctest::Approx(0.55f));
}

TEST_CASE("settings: out-of-range values clamp on load") {
    tng::Settings s;
    REQUIRE(s.fromJson(R"({"bufferFrames": 48000, "pitchRangePct": 13.0,
                            "xfCurve": 9, "masterGain": 1.5})"));
    CHECK(s.bufferFrames == 1024);
    CHECK(s.pitchRangePct == 16.0f);
    CHECK(s.xfCurve == 2);
    CHECK(s.masterGain == doctest::Approx(1.0f));

    tng::Settings lo;
    REQUIRE(lo.fromJson(R"({"bufferFrames": 16, "pitchRangePct": 8.0,
                             "xfCurve": -3, "masterGain": -0.2})"));
    CHECK(lo.bufferFrames == 64);
    CHECK(lo.pitchRangePct == 10.0f);
    CHECK(lo.xfCurve == 0);
    CHECK(lo.masterGain == doctest::Approx(0.0f));
}

TEST_CASE("settings: missing fields keep per-field defaults") {
    tng::Settings s;
    REQUIRE(s.fromJson(R"({"bootMixMode": true})"));
    CHECK(s.bootMixMode);
    CHECK(s.bufferFrames == 256);
    CHECK(s.xfCurve == 1);
    CHECK(s.masterGain == doctest::Approx(0.7f));
}
