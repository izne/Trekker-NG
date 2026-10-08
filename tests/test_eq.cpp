// EQ tests (SPEC §4.6, M5a): knob curve, flat transparency, band kill and
// boost through the LR4 crossover split in Eq3, plus one end-to-end case
// through the Mixer.

#include "doctest.h"

#include "eq.h"
#include "mixer.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr float kFs = 44100.0f;

// Runs `seconds` of a sine at `freq` through eq on channel 0, skipping the
// first `warm` seconds as settle time (knob smoothing + crossover transient);
// returns the RMS of the tail. A sine of amplitude a has RMS a/sqrt(2).
float eqRms(tng::Eq3& eq, float freq, float amp, double seconds, double warm) {
    const int block = 256;
    const long total = static_cast<long>(seconds * kFs);
    const long skip = static_cast<long>(warm * kFs);
    double sum = 0.0;
    long seen = 0;
    long idx = 0;
    while (idx < total) {
        eq.beginBlock(block);
        const long end = std::min(total, idx + block);
        for (; idx < end; ++idx) {
            const float x = amp * std::sin(6.28318530718f * freq *
                                           static_cast<float>(idx) / kFs);
            const float y = eq.process(0, x);
            if (idx >= skip) {
                sum += static_cast<double>(y) * y;
                ++seen;
            }
        }
    }
    return seen > 0 ? static_cast<float>(std::sqrt(sum / seen)) : 0.0f;
}

} // namespace

TEST_CASE("eq: Traktor-style knob curve") {
    using tng::Eq3;
    CHECK(Eq3::curve(0.5f) == doctest::Approx(1.0f).epsilon(1e-6));
    CHECK(Eq3::curve(0.0f) == 0.0f); // full CCW = kill
    CHECK(Eq3::curve(1.0f) ==
          doctest::Approx(std::pow(10.0f, 6.0f / 20.0f)).epsilon(1e-5));
    CHECK(Eq3::curve(0.75f) ==
          doctest::Approx(std::pow(10.0f, 3.0f / 20.0f)).epsilon(1e-5));
    CHECK(Eq3::curve(0.25f) == doctest::Approx(0.125f).epsilon(1e-6));

    // Out-of-range knobs clamp, and the curve never dips below the kill.
    CHECK(Eq3::curve(-1.0f) == 0.0f);
    CHECK(Eq3::curve(2.0f) == doctest::Approx(Eq3::curve(1.0f)));

    // Monotonic across the whole travel (no dips or bumps).
    float prev = 0.0f;
    for (int i = 0; i <= 100; ++i) {
        const float g = Eq3::curve(static_cast<float>(i) / 100.0f);
        CHECK(g >= prev);
        prev = g;
    }
}

TEST_CASE("eq: flat knobs are magnitude-transparent") {
    tng::Eq3 eq;
    eq.setSampleRate(44100);

    const float amp = 0.5f;
    const float in = eqRms(eq, 440.0f, amp, 0.4, 0.2); // flat defaults
    const float want = amp / std::sqrt(2.0f);
    CHECK(in == doctest::Approx(want).epsilon(0.01));
    // The all-pass recombination shifts phase but keeps the level: out == in.
    // (Re-run on a fresh instance to compare out vs in directly.)
    tng::Eq3 eq2;
    eq2.setSampleRate(44100);
    const float out = eqRms(eq2, 440.0f, amp, 0.4, 0.2);
    CHECK(out == doctest::Approx(in).epsilon(0.01));
}

TEST_CASE("eq: killing a band attenuates its in-band sine") {
    tng::Eq3 eq;
    eq.setSampleRate(44100);

    const float amp = 0.5f;
    // LR4 roll-off: attenuation is set by how deep the feed sits inside the
    // killed band (50 Hz is 2.3 oct below the 250 Hz split, 12 kHz is 1.6
    // oct above the 4 kHz split).
    SUBCASE("low band kills a 50 Hz sine by 40+ dB") {
        eq.setTarget(0, 0.0f);
        const float rms = eqRms(eq, 50.0f, amp, 0.6, 0.3);
        CHECK(rms < amp / std::sqrt(2.0f) * 0.01f);
    }
    SUBCASE("mid band kills a 1000 Hz sine by 40+ dB") {
        eq.setTarget(1, 0.0f);
        const float rms = eqRms(eq, 1000.0f, amp, 0.6, 0.3);
        CHECK(rms < amp / std::sqrt(2.0f) * 0.01f);
    }
    SUBCASE("high band kills a 12 kHz sine by 30+ dB") {
        eq.setTarget(2, 0.0f);
        const float rms = eqRms(eq, 12000.0f, amp, 0.6, 0.3);
        CHECK(rms < amp / std::sqrt(2.0f) * 0.0316f);
    }
}

TEST_CASE("eq: killing one band spares the others") {
    tng::Eq3 eq;
    eq.setSampleRate(44100);

    const float amp = 0.5f;
    const float want = amp / std::sqrt(2.0f); // flat level
    SUBCASE("low kill leaves a 1000 Hz sine alone") {
        eq.setTarget(0, 0.0f);
        const float rms = eqRms(eq, 1000.0f, amp, 0.6, 0.3);
        CHECK(rms == doctest::Approx(want).epsilon(0.05));
    }
    SUBCASE("mid kill leaves 60 Hz and 8 kHz alone") {
        eq.setTarget(1, 0.0f);
        CHECK(eqRms(eq, 60.0f, amp, 0.6, 0.3) ==
              doctest::Approx(want).epsilon(0.05));
        tng::Eq3 eq2;
        eq2.setSampleRate(44100);
        eq2.setTarget(1, 0.0f);
        CHECK(eqRms(eq2, 8000.0f, amp, 0.6, 0.3) ==
              doctest::Approx(want).epsilon(0.05));
    }
    SUBCASE("high kill leaves a 1000 Hz sine alone") {
        eq.setTarget(2, 0.0f);
        const float rms = eqRms(eq, 1000.0f, amp, 0.6, 0.3);
        CHECK(rms == doctest::Approx(want).epsilon(0.05));
    }
}

TEST_CASE("eq: full CW boosts the band +6 dB") {
    tng::Eq3 eq;
    eq.setSampleRate(44100);

    const float amp = 0.3f;
    const float want = amp / std::sqrt(2.0f) * std::pow(10.0f, 6.0f / 20.0f);

    SUBCASE("low band at 60 Hz") {
        eq.setTarget(0, 1.0f);
        CHECK(eqRms(eq, 60.0f, amp, 0.6, 0.3) ==
              doctest::Approx(want).epsilon(0.05));
    }
    SUBCASE("mid band at 1000 Hz") {
        eq.setTarget(1, 1.0f);
        CHECK(eqRms(eq, 1000.0f, amp, 0.6, 0.3) ==
              doctest::Approx(want).epsilon(0.05));
    }
    SUBCASE("high band at 10 kHz") {
        eq.setTarget(2, 1.0f);
        CHECK(eqRms(eq, 10000.0f, amp, 0.6, 0.3) ==
              doctest::Approx(want).epsilon(0.05));
    }
}

TEST_CASE("mixer: eq knobs clamp and report") {
    tng::Mixer mixer;
    CHECK(mixer.eqKnob(0, 0) == 0.5f);
    CHECK(mixer.eqKnob(1, 2) == 0.5f);

    mixer.setEqKnob(0, 1, 0.25f);
    CHECK(mixer.eqKnob(0, 1) == 0.25f);
    CHECK(mixer.eqKnob(1, 1) == 0.5f); // other deck untouched

    mixer.setEqKnob(0, 1, 2.0f);
    CHECK(mixer.eqKnob(0, 1) == 1.0f);
    mixer.setEqKnob(0, 1, -1.0f);
    CHECK(mixer.eqKnob(0, 1) == 0.0f);

    mixer.setEqKnob(9, 0, 0.1f); // out-of-range deck/band: ignored
    mixer.setEqKnob(0, 9, 0.1f);
    CHECK(mixer.eqKnob(9, 0) == 0.5f);
    CHECK(mixer.eqKnob(0, 9) == 0.5f);
}

TEST_CASE("mixer: eq kill silences the deck through the full path") {
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setCrossfader(-1.0f); // deck A full, deck B silent
    mixer.setEqKnob(0, 0, 0.0f); // kill deck A's low band

    const int frames = 256;
    const long total = static_cast<long>(0.6f * kFs);
    const long skip = static_cast<long>(0.3f * kFs);
    const float amp = 0.5f;
    std::vector<float> a(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    double sum = 0.0;
    long seen = 0;
    for (long base = 0; base < total; base += frames) {
        for (int i = 0; i < frames; ++i) {
            const float x = amp * std::sin(6.28318530718f * 50.0f *
                                           static_cast<float>(base + i) / kFs);
            a[static_cast<size_t>(i) * 2] = x;
            a[static_cast<size_t>(i) * 2 + 1] = x;
        }
        mixer.process(a.data(), b.data(), out.data(), frames);
        for (int i = 0; i < frames; ++i) {
            const long pos = base + i;
            if (pos >= skip && pos < total) {
                const float y = out[static_cast<size_t>(i) * 2];
                sum += static_cast<double>(y) * y;
                ++seen;
            }
        }
    }
    const float rms = seen > 0 ? static_cast<float>(std::sqrt(sum / seen)) : 0.0f;
    CHECK(rms < 1e-3f);
}
