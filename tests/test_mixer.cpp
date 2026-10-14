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

// --- M10 headphone cue (SPEC §4.8 / §9 test 10) -------------------------------

TEST_CASE("mixer: PFL is pre-fader (M10)") {
    // Deck A's fader is fully closed (line gain 0, crossfader parked on B)
    // yet its PFL still feeds the cue bus: the tap sits post-EQ, pre-fader
    // and pre-crossfader, so a DJ can cue a deck that is not audible.
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setSampleRate(44100);
    mixer.setCrossfader(1.0f);  // master hears only B ...
    mixer.setLineGain(0, 0.0f); // ... and A's own fader is closed too
    mixer.setPfl(0, true);
    mixer.setPhonesMix(0.0f); // phones = cue bus only

    const int frames = 32;
    std::vector<float> a(static_cast<size_t>(frames) * 2, 0.5f);
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> out(static_cast<size_t>(frames) * 2, -1.0f);
    std::vector<float> cue(static_cast<size_t>(frames) * 2, -1.0f);
    for (int i = 0; i < 24; ++i) {
        mixer.process(a.data(), b.data(), out.data(), frames, cue.data());
    }

    CHECK(out[0] == 0.0f); // A fully out of the master, B silent: exact zero
    // A's post-EQ signal at unity in the phones - faders ignored.
    CHECK(cue[0] == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(cue[1] == doctest::Approx(0.5f).epsilon(1e-4));

    // Same proof against the line fader alone: crossfader centered (A at
    // equal power) but line A closed - master silent, cue unchanged.
    mixer.setCrossfader(-1.0f);
    mixer.setLineGain(0, 0.0f);
    for (int i = 0; i < 24; ++i) {
        mixer.process(a.data(), b.data(), out.data(), frames, cue.data());
    }
    CHECK(out[0] == 0.0f);
    CHECK(cue[0] == doctest::Approx(0.5f).epsilon(1e-4));
}

TEST_CASE("mixer: PFL tap is post-EQ (M10)") {
    // Killing deck A's low band removes it from the cue bus too - the tap
    // sees the EQ output, not the raw deck signal.
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setSampleRate(44100);
    mixer.setCrossfader(-1.0f);
    mixer.setLineGain(0, 1.0f);
    mixer.setEqKnob(0, 0, 0.0f); // deck A low kill (full CCW = exact silence)
    mixer.setPfl(0, true);
    mixer.setPhonesMix(0.0f);

    const int frames = 32;
    std::vector<float> a(static_cast<size_t>(frames) * 2, 0.5f); // 0 Hz = low band
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> cue(static_cast<size_t>(frames) * 2, -1.0f);
    // The knob smoother (kTauSec = 12 ms) plus the crossover need a while:
    // 150 blocks * 32 frames ~= 109 ms at 44.1 kHz.
    for (int i = 0; i < 150; ++i) {
        mixer.process(a.data(), b.data(), out.data(), frames, cue.data());
    }
    CHECK(std::fabs(cue[0]) < 1e-3f); // DC never reaches the phones
    CHECK(std::fabs(out[0]) < 1e-3f); // ...nor the master

    // Control: flat EQ, the same DC lands in the cue bus at unity.
    mixer.setEqKnob(0, 0, 0.5f);
    for (int i = 0; i < 150; ++i) {
        mixer.process(a.data(), b.data(), out.data(), frames, cue.data());
    }
    CHECK(cue[0] == doctest::Approx(0.5f).epsilon(1e-3));
}

TEST_CASE("mixer: PFL ramps instead of stepping (M10)") {
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setSampleRate(44100);
    mixer.setCrossfader(-1.0f);
    mixer.setPhonesMix(0.0f);

    const int frames = 32;
    std::vector<float> a(static_cast<size_t>(frames) * 2, 0.5f);
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> cue(static_cast<size_t>(frames) * 2, 0.0f);
    for (int i = 0; i < 24; ++i) {
        mixer.process(a.data(), b.data(), out.data(), frames, cue.data());
    }
    CHECK(cue[0] == doctest::Approx(0.0f).epsilon(1e-5)); // PFL off: silent

    mixer.setPfl(0, true); // jump: the ramp must cross the whole block
    mixer.process(a.data(), b.data(), out.data(), frames, cue.data());

    CHECK(cue[0] < 0.05f); // first frame still nearly silent - no click
    CHECK(cue[(frames - 1) * 2] == doctest::Approx(0.5f).epsilon(1e-4));
    for (int i = 1; i < frames; ++i) {
        CHECK(cue[static_cast<size_t>(i) * 2] >=
              cue[static_cast<size_t>(i - 1) * 2] - 1e-5f); // monotonic rise
    }

    // And back down when released - the same ramp, mirrored.
    mixer.setPfl(0, false);
    mixer.process(a.data(), b.data(), out.data(), frames, cue.data());
    CHECK(cue[0] > 0.45f);
    CHECK(std::fabs(cue[(frames - 1) * 2]) < 1e-5f);
    for (int i = 1; i < frames; ++i) {
        CHECK(cue[static_cast<size_t>(i) * 2] <=
              cue[static_cast<size_t>(i - 1) * 2] + 1e-5f);
    }
}

TEST_CASE("mixer: PHONES blends cue and master (M10)") {
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setSampleRate(44100);
    mixer.setCrossfader(-1.0f);
    mixer.setLineGain(0, 0.5f); // master = 0.25, cue bus = 0.5 (distinct)
    mixer.setPfl(0, true);

    const int frames = 32;
    std::vector<float> a(static_cast<size_t>(frames) * 2, 0.5f);
    std::vector<float> b(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> cue(static_cast<size_t>(frames) * 2, 0.0f);

    // 0 = cue bus only.
    mixer.setPhonesMix(0.0f);
    for (int i = 0; i < 24; ++i) mixer.process(a.data(), b.data(), out.data(), frames, cue.data());
    CHECK(cue[0] == doctest::Approx(0.5f).epsilon(1e-4));

    // 1 = master only, bit-exact (both under the 0.9 knee).
    mixer.setPhonesMix(1.0f);
    for (int i = 0; i < 24; ++i) mixer.process(a.data(), b.data(), out.data(), frames, cue.data());
    CHECK(cue[0] == out[0]); // exact float equality, not an approximation
    CHECK(cue[1] == out[1]);

    // 0.5 = even blend of the two.
    mixer.setPhonesMix(0.5f);
    for (int i = 0; i < 24; ++i) mixer.process(a.data(), b.data(), out.data(), frames, cue.data());
    CHECK(cue[0] == doctest::Approx(0.375f).epsilon(1e-4)); // (0.5 + 0.25) / 2

    // Setter clamps like the other mixer knobs; PFL state round-trips.
    mixer.setPhonesMix(4.0f);
    CHECK(mixer.phonesMix() == 1.0f);
    mixer.setPhonesMix(-1.0f);
    CHECK(mixer.phonesMix() == 0.0f);
    CHECK(mixer.pfl(0));  // set on above, stays on
    CHECK(!mixer.pfl(1)); // B untouched so far
    mixer.setPfl(1, true);
    CHECK(mixer.pfl(1));
    mixer.setPfl(1, false);
    CHECK(!mixer.pfl(1));
}

TEST_CASE("mixer: cueOut == nullptr keeps the pre-M10 output (M10)") {
    // Two identically configured mixers, one rendering the cue bus and one
    // not: the master outputs must match sample for sample - producing
    // phones audio never perturbs what the master device plays (SPEC §4.8,
    // "features off = exactly today's behavior").
    const int frames = 32;
    tng::Mixer plain;
    tng::Mixer cuey;
    for (tng::Mixer* m : {&plain, &cuey}) {
        m->setMasterGain(1.0f);
        m->setSampleRate(44100);
        m->setCrossfader(-0.35f);
        m->setLineGain(0, 0.7f);
        m->setEqKnob(1, 2, 0.8f); // unequal state on purpose
        m->setPfl(0, true);       // PFL on in both - still must not leak
        m->setPfl(1, false);
        m->setPhonesMix(0.5f);
    }

    std::vector<float> a(static_cast<size_t>(frames) * 2);
    std::vector<float> b(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames * 2; ++i) { // arbitrary non-DC content
        a[static_cast<size_t>(i)] = std::sin(static_cast<float>(i) * 0.37f) * 0.4f;
        b[static_cast<size_t>(i)] = std::cos(static_cast<float>(i) * 0.21f) * 0.4f;
    }
    std::vector<float> outA(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> outB(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> cue(static_cast<size_t>(frames) * 2, 0.0f);

    for (int i = 0; i < 10; ++i) {
        plain.process(a.data(), b.data(), outA.data(), frames);
        cuey.process(a.data(), b.data(), outB.data(), frames, cue.data());
        for (int k = 0; k < frames * 2; ++k) {
            CHECK(outA[static_cast<size_t>(k)] == outB[static_cast<size_t>(k)]);
        }
    }
}

TEST_CASE("mixer: single-deck mode cue tap (M10)") {
    // The console path (deckB == nullptr) renders with PFL too: the tap
    // reads the deck sample before `out` is written, so deckA may alias the
    // output buffer (as the console's offline render does).
    tng::Mixer mixer;
    mixer.setMasterGain(1.0f);
    mixer.setSampleRate(44100);
    mixer.setLineGain(0, 1.0f);
    mixer.setPfl(0, true);
    mixer.setPhonesMix(0.0f);

    const int frames = 32;
    std::vector<float> in(static_cast<size_t>(frames) * 2, 0.5f);
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    std::vector<float> cue(static_cast<size_t>(frames) * 2, 0.0f);
    for (int i = 0; i < 24; ++i) {
        mixer.process(in.data(), nullptr, out.data(), frames, cue.data());
    }
    CHECK(out[0] == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(cue[0] == doctest::Approx(0.5f).epsilon(1e-4)); // pre-line-gain: full

    // Line gain closed: master silent, cue keeps the pre-fader signal.
    mixer.setLineGain(0, 0.0f);
    for (int i = 0; i < 24; ++i) {
        mixer.process(in.data(), nullptr, out.data(), frames, cue.data());
    }
    CHECK(out[0] == 0.0f);
    CHECK(cue[0] == doctest::Approx(0.5f).epsilon(1e-4));

    // Aliased render: deckA == out (console --render pattern). A fresh
    // mixer keeps the gain ramp stationary (target == its 1.0 start), so
    // the buffer genuinely carries the input through - any read-after-write
    // in process() would corrupt it.
    tng::Mixer render;
    render.setMasterGain(1.0f);
    render.setSampleRate(44100);
    render.setPfl(0, true);
    render.setPhonesMix(0.0f);
    std::vector<float> aliased = in;
    for (int i = 0; i < 24; ++i) {
        render.process(aliased.data(), nullptr, aliased.data(), frames, cue.data());
    }
    CHECK(aliased[0] == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(aliased[1] == doctest::Approx(0.5f).epsilon(1e-4));
    CHECK(cue[0] == doctest::Approx(0.5f).epsilon(1e-4));
}
