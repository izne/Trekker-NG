// Mixer tests (SPEC §4.6): line gains, equal-power crossfader curve, master
// gain, clamp and the single-deck (deckB == nullptr) bypass added in M4a.

#include "doctest.h"

#include "mixer.h"

#include <cmath>
#include <vector>

namespace {

// Runs two blocks and returns the steady-state gain of `channel` (0 = A,
// 1 = B): block 1 ramps the internal gain from its previous value to the new
// target, block 2 runs at the constant target. Input is unit DC on the
// measured channel and silence on the other, master gain is 1.
float steadyGain(tng::Mixer& mixer, int deck, float deckBdc, float* outSample = nullptr) {
    const int frames = 32;
    std::vector<float> a(static_cast<size_t>(frames) * 2, deck == 0 ? 1.0f : 0.0f);
    std::vector<float> b(static_cast<size_t>(frames) * 2, deckBdc);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);

    mixer.process(a.data(), b.data(), out.data(), frames); // ramp block
    mixer.process(a.data(), b.data(), out.data(), frames); // steady block
    if (outSample != nullptr) *outSample = out[0];
    return out[0]; // left channel of the steady block (block 2, frame 0)
}

} // namespace

TEST_CASE("mixer: equal-power crossfader curve") {
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);

    // Gain of each deck alone at crossfader positions -1, -0.5, 0, 0.5, 1.
    const float positions[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    for (float xf : positions) {
        mixer.setCrossfader(xf);
        const float ga = steadyGain(mixer, 0, 0.0f);
        const float gb = steadyGain(mixer, 1, 1.0f);

        const float angle = (xf + 1.0f) * 0.78539816339f;
        const float wantA = std::cos(angle);
        const float wantB = std::sin(angle);
        CHECK(ga == doctest::Approx(wantA).epsilon(1e-4));
        CHECK(gb == doctest::Approx(wantB).epsilon(1e-4));

        // Equal power: ga^2 + gb^2 == 1 at every position (SPEC §4.6).
        CHECK(ga * ga + gb * gb == doctest::Approx(1.0f).epsilon(1e-4));
    }

    // Endpoints are exact: full A, then full B.
    mixer.setCrossfader(-1.0f);
    CHECK(steadyGain(mixer, 0, 0.0f) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(steadyGain(mixer, 1, 1.0f) == doctest::Approx(0.0f).epsilon(1e-4));
    mixer.setCrossfader(1.0f);
    CHECK(steadyGain(mixer, 0, 0.0f) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(steadyGain(mixer, 1, 1.0f) == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("mixer: line gains multiply the crossfade gains") {
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setCrossfader(-1.0f); // deck A at full, deck B silent
    mixer.setLineGain(0, 0.5f);
    CHECK(steadyGain(mixer, 0, 0.0f) == doctest::Approx(0.5f).epsilon(1e-4));

    mixer.setCrossfader(1.0f); // deck B at full
    mixer.setLineGain(1, 0.25f);
    CHECK(steadyGain(mixer, 1, 1.0f) == doctest::Approx(0.25f).epsilon(1e-4));

    // Out-of-range setpoints clamp to 0..1.
    mixer.setLineGain(1, 2.0f);
    CHECK(mixer.lineGain(1) == 1.0f);
    mixer.setLineGain(1, -1.0f);
    CHECK(mixer.lineGain(1) == 0.0f);
    mixer.setCrossfader(5.0f);
    CHECK(mixer.crossfader() == 1.0f);
    mixer.setCrossfader(-5.0f);
    CHECK(mixer.crossfader() == -1.0f);
}

TEST_CASE("mixer: master gain and clamp") {
    tng::Mixer mixer;
    mixer.setCrossfader(-1.0f);
    mixer.setMasterGain(0.7f);
    CHECK(steadyGain(mixer, 0, 0.0f) == doctest::Approx(0.7f).epsilon(1e-4));

    // Summed decks over unity are clamped to +-1 (hard clamp until the M5
    // soft limiter, SPEC §4.6).
    mixer.setMasterGain(1.0f);
    mixer.setCrossfader(0.0f); // both decks at 0.707
    float outSample = 0.0f;
    steadyGain(mixer, 0, 1.0f, &outSample); // A = 1, B = 1 -> 1.414 before clamp
    CHECK(outSample == 1.0f);
}

TEST_CASE("mixer: single-deck mode bypasses the crossfader") {
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setCrossfader(0.0f); // would be 0.707 in two-deck mode

    const int frames = 32;
    std::vector<float> a(static_cast<size_t>(frames) * 2, 1.0f);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    mixer.process(a.data(), nullptr, out.data(), frames);
    mixer.process(a.data(), nullptr, out.data(), frames);

    // deckA passes at full line gain regardless of crossfader position -
    // this keeps the console and the M3 UI at their original loudness.
    CHECK(out[0] == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(out[1] == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("mixer: fader moves ramp instead of stepping") {
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setCrossfader(-1.0f);

    const int frames = 32;
    std::vector<float> a(static_cast<size_t>(frames) * 2, 1.0f);
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f); // B silent: out = A's gain
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    mixer.process(a.data(), b.data(), out.data(), frames);
    mixer.process(a.data(), b.data(), out.data(), frames); // settled at full A
    CHECK(out[0] == doctest::Approx(1.0f).epsilon(1e-4));

    mixer.setCrossfader(1.0f); // jump: A must ramp down across the block
    mixer.process(a.data(), b.data(), out.data(), frames);

    // First frame still near the old gain, last frame on the new target,
    // and monotonic in between (no mid-block step).
    CHECK(out[0] > 0.9f);
    CHECK(std::fabs(out[(frames - 1) * 2]) < 1e-5f);
    for (int i = 1; i < frames; ++i) {
        CHECK(out[static_cast<size_t>(i) * 2] <= out[static_cast<size_t>(i - 1) * 2] + 1e-5f);
    }
}
