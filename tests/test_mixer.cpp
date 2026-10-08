// Mixer tests (SPEC §4.6): line gains, the M5b crossfader curves, master
// gain, the M5b soft clip and the single-deck (deckB == nullptr) bypass
// added in M4a.

#include "doctest.h"

#include "mixer.h"

#include <cmath>
#include <vector>

namespace {

// Runs the block until everything settles and returns the steady-state gain
// of `channel` (0 = A, 1 = B): the first blocks ramp the mixer's internal
// gain to the new target, and the M5a EQ crossover (LR4 at 250 Hz) needs a
// few ms of DC before its output stops moving. Input is half-unit DC on the
// measured channel and half on the other (M5b: 0.5 keeps every measurement
// under the 0.9 soft-clip knee, where the signal is bit-exact; the result
// is scaled back so the numbers read as pure gain - callers use master
// gain 1.0, or check gain * master).
float steadyGain(tng::Mixer& mixer, int deck, float deckBdc, float* outSample = nullptr) {
    const int frames = 32;
    std::vector<float> a(static_cast<size_t>(frames) * 2, deck == 0 ? 0.5f : 0.0f);
    std::vector<float> b(static_cast<size_t>(frames) * 2, deckBdc * 0.5f);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);

    for (int i = 0; i < 24; ++i) mixer.process(a.data(), b.data(), out.data(), frames);
    if (outSample != nullptr) *outSample = out[0] * 2.0f;
    return out[0] * 2.0f; // left channel of the last (settled) block, frame 0
}

} // namespace

TEST_CASE("mixer: crossfader curves (M5b)") {
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    const float positions[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};

    SUBCASE("constant power (default)") {
        CHECK(mixer.xfCurve() == tng::Mixer::kXfConstantPower);

        // Gain of each deck alone at crossfader positions -1..1.
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
    }

    SUBCASE("linear") {
        mixer.setXfCurve(tng::Mixer::kXfLinear);
        for (float xf : positions) {
            mixer.setCrossfader(xf);
            const float t = (xf + 1.0f) * 0.5f;
            CHECK(steadyGain(mixer, 0, 0.0f) == doctest::Approx(1.0f - t).epsilon(1e-4));
            CHECK(steadyGain(mixer, 1, 1.0f) == doctest::Approx(t).epsilon(1e-4));
        }
    }

    SUBCASE("sharp cut: full until the last 5%") {
        mixer.setXfCurve(tng::Mixer::kXfSharpCut);
        // Most of the travel (t <= 0.95): both decks at full.
        const float mid[] = {-0.9f, 0.0f, 0.9f};
        for (float xf : mid) {
            mixer.setCrossfader(xf);
            CHECK(steadyGain(mixer, 0, 0.0f) == doctest::Approx(1.0f).epsilon(1e-4));
            CHECK(steadyGain(mixer, 1, 1.0f) == doctest::Approx(1.0f).epsilon(1e-4));
        }
        // xf = 0.94 -> t = 0.97 -> ga = (1 - t) * 20 = 0.6.
        mixer.setCrossfader(0.94f);
        CHECK(steadyGain(mixer, 0, 0.0f) == doctest::Approx(0.6f).epsilon(1e-4));
        CHECK(steadyGain(mixer, 1, 1.0f) == doctest::Approx(1.0f).epsilon(1e-4));
        // Endpoints are exact: full A, then full B.
        mixer.setCrossfader(-1.0f);
        CHECK(steadyGain(mixer, 0, 0.0f) == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(steadyGain(mixer, 1, 1.0f) == doctest::Approx(0.0f).epsilon(1e-4));
        mixer.setCrossfader(1.0f);
        CHECK(steadyGain(mixer, 0, 0.0f) == doctest::Approx(0.0f).epsilon(1e-4));
        CHECK(steadyGain(mixer, 1, 1.0f) == doctest::Approx(1.0f).epsilon(1e-4));
    }

    SUBCASE("curve setter clamps out-of-range ids") {
        mixer.setXfCurve(99);
        CHECK(mixer.xfCurve() == tng::Mixer::kXfSharpCut);
        mixer.setXfCurve(-1);
        CHECK(mixer.xfCurve() == tng::Mixer::kXfLinear);
    }
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

TEST_CASE("mixer: master gain and soft clip (M5b)") {
    tng::Mixer mixer;
    mixer.setCrossfader(-1.0f);
    mixer.setMasterGain(0.7f);
    CHECK(steadyGain(mixer, 0, 0.0f) == doctest::Approx(0.7f).epsilon(1e-4));
    CHECK(!mixer.takeClipFlag()); // 0.7 stays under the 0.9 knee

    // Summed decks over unity go through the tanh knee: never +-1, always
    // just below it, and the one-shot clip flag latches for the UI light.
    mixer.setMasterGain(1.0f);
    mixer.setCrossfader(0.0f); // both decks at 0.707
    steadyGain(mixer, 0, 1.0f); // A = 0.5, B = 0.5 -> 0.707, still under
    CHECK(!mixer.takeClipFlag());
    const int frames = 32;
    std::vector<float> a(static_cast<size_t>(frames) * 2, 1.0f);
    std::vector<float> b(static_cast<size_t>(frames) * 2, 1.0f);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    for (int i = 0; i < 24; ++i) mixer.process(a.data(), b.data(), out.data(), frames);
    const float clipped = out[0]; // 0.707 + 0.707 = 1.414 into the knee
    CHECK(clipped < 1.0f);
    CHECK(clipped > 0.99f);
    CHECK(mixer.takeClipFlag()); // latched ...
    CHECK(!mixer.takeClipFlag()); // ... and consumed by the read
}

TEST_CASE("mixer: single-deck mode bypasses the crossfader") {
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setCrossfader(0.0f); // would be 0.707 in two-deck mode

    const int frames = 32;
    // 0.5 DC: under the 0.9 knee, so the measurement is bit-exact.
    std::vector<float> a(static_cast<size_t>(frames) * 2, 0.5f);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    mixer.process(a.data(), nullptr, out.data(), frames);
    mixer.process(a.data(), nullptr, out.data(), frames);

    // deckA passes at full line gain regardless of crossfader position -
    // this keeps the console and the M3 UI at their original loudness.
    CHECK(out[0] == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(out[1] == doctest::Approx(0.5f).epsilon(1e-4));
}

TEST_CASE("mixer: fader moves ramp instead of stepping") {
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setCrossfader(-1.0f);

    const int frames = 32;
    // 0.5 DC: under the 0.9 soft-clip knee, so the levels read as pure gain.
    std::vector<float> a(static_cast<size_t>(frames) * 2, 0.5f);
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f); // B silent: out = A's gain
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    // Settle the gain ramp and the M5a EQ crossover before measuring.
    for (int i = 0; i < 24; ++i) mixer.process(a.data(), b.data(), out.data(), frames);
    CHECK(out[0] == doctest::Approx(0.5f).epsilon(1e-4));

    mixer.setCrossfader(1.0f); // jump: A must ramp down across the block
    mixer.process(a.data(), b.data(), out.data(), frames);

    // First frame still near the old gain, last frame on the new target,
    // and monotonic in between (no mid-block step).
    CHECK(out[0] > 0.45f);
    CHECK(std::fabs(out[(frames - 1) * 2]) < 1e-5f);
    for (int i = 1; i < frames; ++i) {
        CHECK(out[static_cast<size_t>(i) * 2] <= out[static_cast<size_t>(i - 1) * 2] + 1e-5f);
    }
}
