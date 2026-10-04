// Cue persistence (SPEC §4.5): meta.json "cues" parse/format helpers.
// String-based only - the suite never touches the filesystem (testutil.h).

#include "doctest.h"
#include "track_loader.h"

#include <string>
#include <vector>

namespace {

// Parse helper: returns the cues and requires the call to succeed.
std::vector<tng::Cue> parsed(const std::string& text, bool* ok = nullptr) {
    std::vector<tng::Cue> cues;
    std::string error;
    const bool good = tng::parseCuesFromMeta(text, cues, &error);
    if (ok) *ok = good;
    if (good) CHECK(error.empty());
    return cues;
}

} // namespace

TEST_CASE("loader: cues parse from meta.json (SPEC 4.5)") {
    const std::string meta = R"({
        "title": "T",
        "cues": [
            { "name": "Intro", "position_ms": 0 },
            { "name": "Drop", "position_ms": 61250 },
            "not-an-object",
            { "name": "bad-type", "position_ms": "later" },
            { "name": "neg", "position_ms": -5 }
        ]
    })";

    bool ok = false;
    const auto cues = parsed(meta, &ok);
    REQUIRE(ok);
    // Non-object entries are skipped; wrong field types keep their defaults;
    // negative positions clamp to 0.
    REQUIRE(cues.size() == 4);
    CHECK(cues[0].name == "Intro");
    CHECK(cues[0].positionMs == doctest::Approx(0.0));
    CHECK(cues[1].name == "Drop");
    CHECK(cues[1].positionMs == doctest::Approx(61250.0));
    CHECK(cues[2].name == "bad-type");
    CHECK(cues[2].positionMs == doctest::Approx(0.0));
    CHECK(cues[3].name == "neg");
    CHECK(cues[3].positionMs == doctest::Approx(0.0));

    // Absent "cues" array: the output stays untouched.
    std::vector<tng::Cue> out = {{"sentinel", 123.0}};
    std::string error;
    REQUIRE(tng::parseCuesFromMeta(R"({"title": "x"})", out, &error));
    REQUIRE(out.size() == 1);
    CHECK(out[0].name == "sentinel");

    // Not JSON at all: false, error set, output untouched.
    REQUIRE_FALSE(tng::parseCuesFromMeta("not json", out, &error));
    CHECK_FALSE(error.empty());
    REQUIRE(out.size() == 1);
}

TEST_CASE("loader: setCuesInMeta replaces cues and keeps every other field") {
    const std::string meta = R"({
        "format_version": 1,
        "title": "Keep me",
        "custom_field": { "x": 1 },
        "cues": [{ "name": "old", "position_ms": 1 }]
    })";

    const std::vector<tng::Cue> cues = {{"Intro", 0.0}, {"Drop", 61250.0}};
    std::string outText;
    std::string error;
    REQUIRE(tng::setCuesInMeta(meta, cues, outText, &error));

    // Other fields, including unknown ones (SPEC 5 forward compat), survive.
    CHECK(outText.find("\"Keep me\"") != std::string::npos);
    CHECK(outText.find("custom_field") != std::string::npos);

    // And the new cues parse back with the same values.
    const auto back = parsed(outText);
    REQUIRE(back.size() == 2);
    CHECK(back[0].name == "Intro");
    CHECK(back[0].positionMs == doctest::Approx(0.0));
    CHECK(back[1].name == "Drop");
    CHECK(back[1].positionMs == doctest::Approx(61250.0));

    // Invalid JSON: refused, nothing written.
    outText = "untouched";
    REQUIRE_FALSE(tng::setCuesInMeta("nope", cues, outText, &error));
    CHECK_FALSE(error.empty());
    CHECK(outText == "untouched");
}

TEST_CASE("loader: zip sidecar is the same shape, built from {}") {
    // saveCues() for a .zip writes setCuesInMeta("{}") next to the archive;
    // loadTrack() reads it back through the same cues parsing as here.
    const std::vector<tng::Cue> cues = {{"A", 10.5}, {"B", 2000.0}};
    std::string sidecar;
    std::string error;
    REQUIRE(tng::setCuesInMeta("{}", cues, sidecar, &error));

    const auto back = parsed(sidecar);
    REQUIRE(back.size() == 2);
    CHECK(back[0].name == "A");
    CHECK(back[0].positionMs == doctest::Approx(10.5));
    CHECK(back[1].name == "B");
    CHECK(back[1].positionMs == doctest::Approx(2000.0));
}
