#include "doctest.h"
#include "mixer.h"
#include "testutil.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

using namespace tutil;

// All thresholds are calibrated so a click-free ramp passes with margin while
// a hard toggle/seek (amplitude-sized jumps) fails loudly - SPEC §9.3/§9.4.

TEST_CASE("pitch: 440 Hz at rate 1.10 measures 484 +/- 0.5 Hz (SPEC 9.1)") {
    const uint32_t fs = 44100;
    auto track = makeSineTrack(fs, 10.0, {440.0});
    tng::Deck deck;
    deck.setTrack(std::move(track));
    deck.setRate(1.10);
    deck.setPlaying(true);

    const auto out = renderFrames(deck, static_cast<int64_t>(5.0 * fs));
    const double f = measureFreq(out, static_cast<int64_t>(1.0 * fs),
                                 static_cast<int64_t>(4.0 * fs), fs);
    CHECK(std::fabs(f - 484.0) <= 0.5);
}

TEST_CASE("stem lock: click trains stay locked for 10 min with random rate (SPEC 9.2)") {
    const uint32_t fs = 8000;
    const double seconds = 600.0;
    const int64_t period = 800; // one click per 0.1 s
    auto track = makeClickTrack(fs, seconds, period, /*stemCount=*/2);
    tng::Deck deck;
    deck.setTrack(std::move(track));
    deck.setPlaying(true);

    const int64_t totalFrames = static_cast<int64_t>(seconds * fs);
    const int64_t warmup = 1000; // skip the transport fade-in (first ~40 frames)

    std::vector<float> out(static_cast<size_t>(totalFrames) * 2, 0.0f);

    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> rateDist(0.9, 1.1);
    deck.setRate(rateDist(rng));

    const uint32_t block = 8;
    const int64_t blocks = totalFrames / block;

    // Clicks and their predicted output windows: the playhead crosses source
    // frame C inside block b => the summed impulse peak sits within +/-2
    // frames of that block (Hermite smear).
    std::vector<std::pair<int64_t, int64_t>> windows;
    std::vector<int64_t> clicks;
    for (int64_t c = 0; c < totalFrames; c += period) {
        if (c >= warmup) clicks.push_back(c);
    }

    double prevPos = 0.0;
    size_t nextClick = 0;
    for (int64_t b = 0; b < blocks; ++b) {
        if (b % 560 == 0) deck.setRate(rateDist(rng)); // re-target ~every 0.56 s
        deck.render(out.data() + static_cast<size_t>(b) * block * 2, block);
        const double pos = deck.positionFrames();
        while (nextClick < clicks.size() &&
               static_cast<double>(clicks[nextClick]) <= pos) {
            if (static_cast<double>(clicks[nextClick]) > prevPos) {
                windows.emplace_back(b * block - 2, b * block + block + 2);
            }
            ++nextClick;
        }
        prevPos = pos;
    }

    // Detect click peaks: locked stems sum to >= ~0.98 at the peak of an
    // impulse (worst-case phase at the rate extremes: Hermite evaluated
    // ~0.486/stem, see docs note in the threshold); the buffer has nothing
    // else that big between clicks.
    std::vector<int64_t> peaks;
    for (int64_t i = std::max<int64_t>(warmup - block, 1); i < totalFrames - 1; ++i) {
        const float v = std::fabs(out[static_cast<size_t>(i) * 2]);
        const float vp = std::fabs(out[static_cast<size_t>(i - 1) * 2]);
        const float vn = std::fabs(out[static_cast<size_t>(i + 1) * 2]);
        if (v >= 0.9f && v > vp && v >= vn) peaks.push_back(i);
    }

    // The rate schedule is a random walk, so the final playhead may stop
    // short of (or run into) the track end: expect a window only for the
    // clicks the playhead actually crossed.
    const double finalPos = prevPos;
    size_t expected = 0;
    for (const int64_t c : clicks) {
        if (static_cast<double>(c) <= finalPos) ++expected;
    }
    REQUIRE(expected >= 5900);
    CHECK(windows.size() == expected);
    CHECK(peaks.size() == windows.size());

    // Greedy in-order matching: every predicted window holds exactly one peak.
    size_t p = 0;
    bool matched = true;
    for (const auto& w : windows) {
        while (p < peaks.size() && peaks[p] < w.first) ++p;
        if (p >= peaks.size() || peaks[p] > w.second) {
            matched = false;
            break;
        }
        ++p;
    }
    CHECK(matched);
}

TEST_CASE("mute: stem toggling never jumps the waveform (SPEC 9.3)") {
    const uint32_t fs = 44100;
    auto track = makeSineTrack(fs, 10.0, {440.0});
    tng::Deck deck;
    deck.setTrack(std::move(track));
    deck.setPlaying(true);

    const int64_t fs64 = fs;
    // Let the 5 ms transport fade-in finish before observing steps.
    auto out = renderFrames(deck, fs64 / 2);

    deck.setStem(0, false);
    auto outOff = renderFrames(deck, fs64);
    deck.setStem(0, true);
    auto outOn = renderFrames(deck, fs64);

    // Sine derivative max = 0.5 * 2pi * 440 / 44100 ~= 0.031; the 100 ms
    // mute ramp adds at most rampStep * 0.5 ~= 0.003 per frame.
    const float worst = maxStep(out, 0, fs64 / 2);
    CHECK(worst < 0.05f);
    const float worstOff = maxStep(outOff, 0, fs64);
    CHECK(worstOff < 0.05f);
    const float worstOn = maxStep(outOn, 0, fs64);
    CHECK(worstOn < 0.05f);

    // And the toggle actually did something: silent stretch after the ramp.
    bool sawSilence = false;
    for (int64_t i = fs64 / 4; i < fs64 * 3 / 4; ++i) {
        if (std::fabs(outOff[static_cast<size_t>(i) * 2]) < 1e-6f) {
            sawSilence = true;
            break;
        }
    }
    CHECK(sawSilence);
}

TEST_CASE("cue jump: seek while playing produces no discontinuity (SPEC 9.4)") {
    const uint32_t fs = 44100;
    auto track = makeSineTrack(fs, 10.0, {440.0});
    tng::Deck deck;
    deck.setTrack(std::move(track));
    deck.setPlaying(true);

    const int64_t fs64 = fs;
    auto out = renderFrames(deck, fs64); // 1 s in, transport fade done

    deck.requestSeek(fs64 * 3); // cue jump +3 s
    auto out2 = renderFrames(deck, fs64 / 2);        // crossfade (2 ms) + after

    // Crossfade worst-case step: path difference (<=1.0) blended over 88
    // frames ~= 0.012 + sine derivative 0.031 + gain ramp < 0.003 => < 0.08.
    // A hard cut would show the sine's full span (up to 1.0).
    const float worst = maxStep(out, fs64 * 9 / 10, fs64);
    CHECK(worst < 0.05f);
    const float worstAfter = maxStep(out2, 0, fs64 / 2);
    CHECK(worstAfter < 0.08f);

    // The seek actually happened: playhead is now past 3 s.
    CHECK(deck.positionFrames() >= 3.0 * fs);
}

TEST_CASE("pause and resume are click-free (SPEC 4.5)") {
    const uint32_t fs = 44100;
    auto track = makeSineTrack(fs, 10.0, {440.0});
    tng::Deck deck;
    deck.setTrack(std::move(track));
    deck.setPlaying(true);

    const int64_t fs64 = fs;
    auto out = renderFrames(deck, fs64 / 2); // running, fade-in done

    deck.setPlaying(false);
    auto outPause = renderFrames(deck, fs64 / 2); // 5 ms fade then silence
    deck.setPlaying(true);
    auto outResume = renderFrames(deck, fs64 / 2); // 5 ms fade back in

    CHECK(maxStep(out, fs64 / 4, fs64 / 2) < 0.05f);
    CHECK(maxStep(outPause, 0, fs64 / 2) < 0.05f);
    CHECK(maxStep(outResume, 0, fs64 / 2) < 0.05f);

    // After the fade completes the paused deck is silent, not holding a click.
    for (int64_t i = fs64 / 4; i < fs64 / 2; ++i) {
        REQUIRE(std::fabs(outPause[static_cast<size_t>(i) * 2]) < 1e-6f);
    }
}

TEST_CASE("allocation counter is armed (sanity for SPEC 9.6)") {
    long count = 0;
    {
        rtcheck::Scope scope;
        std::vector<int> probe(128, 7); // must allocate
        count = scope.count();
        CHECK(probe.size() == 128);
    }
    CHECK(count > 0);
}

TEST_CASE("render path performs no heap allocation (SPEC 9.6)") {
    const uint32_t fs = 44100;
    auto track = makeSineTrack(fs, 10.0, {440.0, 0.0});
    tng::Deck deck;
    deck.setTrack(std::move(track));
    deck.setPlaying(true);

    std::vector<float> buf(256 * 2);

    long count = 0;
    {
        rtcheck::Scope scope;
        for (int i = 0; i < 4; ++i) {
            deck.render(buf.data(), 256);
        }
        deck.requestSeek(static_cast<int64_t>(1.5 * fs));
        deck.render(buf.data(), 256);
        deck.render(buf.data(), 256);
        deck.setStem(0, false);
        deck.render(buf.data(), 256);
        deck.setStem(0, true);
        deck.setRate(1.05);
        deck.render(buf.data(), 256);
        deck.setPlaying(false);
        for (int i = 0; i < 4; ++i) {
            deck.render(buf.data(), 256);
        }
        deck.setPlaying(true);
        deck.render(buf.data(), 256);
        deck.setRate(0.95);
        deck.render(buf.data(), 256);
        // Loop: points set/activated/cleared while rendering (SPEC 4.5).
        deck.setLoop(1000, 5000);
        deck.setLoopActive(true);
        for (int i = 0; i < 4; ++i) {
            deck.render(buf.data(), 256);
        }
        deck.setLoopActive(false);
        deck.clearLoop();
        deck.render(buf.data(), 256);
        count = scope.count();
    }
    CHECK(count == 0);
}

TEST_CASE("hot publish: audio adopts new data, position resets (SPEC 4.7)") {
    const uint32_t fs = 8000;
    tng::Deck deck;
    deck.setTrack(makeSineTrack(fs, 2.0, {440.0}));
    deck.setPlaying(true);
    renderFrames(deck, 1000);
    CHECK(deck.positionFrames() == doctest::Approx(1000.0).epsilon(1e-6));

    deck.publishTrack(makeSineTrack(fs, 3.0, {880.0})); // hot swap while "playing"
    deck.drainRetired(); // not adopted yet: must not free (no observable, no crash)
    REQUIRE(deck.track() != nullptr);
    CHECK(deck.track()->frames == static_cast<int64_t>(3.0 * fs));

    const auto out2 = renderFrames(deck, 500); // adoption happens here
    CHECK(deck.positionFrames() == doctest::Approx(500.0).epsilon(1e-6));
    CHECK(deck.playing()); // transport state carried over
    deck.drainRetired();   // adopted now: retired data released

    // New audio actually plays the new track (880 Hz sine, not silence).
    float peak = 0.0f;
    for (int64_t i = 0; i < 500; ++i) {
        peak = std::max(peak, std::fabs(out2[static_cast<size_t>(i) * 2]));
    }
    CHECK(peak > 0.1f);
}

TEST_CASE("mixer: master gain and hard clamp (SPEC 4.6)") {
    tng::Mixer mixer;
    CHECK(mixer.masterGain() == doctest::Approx(0.7f));

    std::vector<float> buf = {1.0f, -1.0f, 2.0f, -2.0f};
    mixer.process(buf.data(), nullptr, buf.data(), 2);
    CHECK(buf[0] == doctest::Approx(0.7f));
    CHECK(buf[1] == doctest::Approx(-0.7f));
    CHECK(buf[2] == 1.0f);  // clamped, no wrap
    CHECK(buf[3] == -1.0f);

    mixer.setMasterGain(1.0f);
    buf = {1.5f, 0.0f, 0.0f, 0.0f};
    mixer.process(buf.data(), nullptr, buf.data(), 1);
    CHECK(buf[0] == 1.0f);
}

TEST_CASE("loop: playhead wraps at loop out (SPEC 4.5)") {
    const uint32_t fs = 8000;
    auto track = makeSineTrack(fs, 2.0, {440.0});
    tng::Deck deck;
    deck.setTrack(std::move(track));
    REQUIRE(deck.setLoop(1000, 3000));
    deck.setLoopActive(true);
    deck.setPlaying(true);

    renderFrames(deck, 8000); // 1 s: crosses loop out three times

    // Stays inside the loop instead of running toward the track end.
    CHECK(deck.positionFrames() >= 1000.0);
    CHECK(deck.positionFrames() < 3000.0);
    CHECK(deck.playing()); // end-of-track stop never fired
}

TEST_CASE("loop: setLoop rejects bad ranges and keeps state") {
    tng::Deck deck;

    CHECK_FALSE(deck.setLoop(100, 50)); // out <= in
    CHECK_FALSE(deck.setLoop(-1, 500)); // negative in
    CHECK_FALSE(deck.setLoop(10, 10));  // empty range
    CHECK(deck.loopIn() == -1);
    CHECK(deck.loopOut() == -1);

    REQUIRE(deck.setLoop(100, 500));
    CHECK(deck.loopIn() == 100);
    CHECK(deck.loopOut() == 500);

    CHECK_FALSE(deck.setLoop(200, 150)); // rejected: previous points kept
    CHECK(deck.loopIn() == 100);
    CHECK(deck.loopOut() == 500);

    deck.setLoopActive(true);
    CHECK(deck.loopActive());
    deck.setLoopActive(false);
    CHECK_FALSE(deck.loopActive());

    deck.clearLoop();
    CHECK(deck.loopIn() == -1);
    CHECK(deck.loopOut() == -1);
    CHECK_FALSE(deck.loopActive());
}

TEST_CASE("loop: wrap while playing is click-free (SPEC 9.3/9.4)") {
    const uint32_t fs = 44100;
    auto track = makeSineTrack(fs, 10.0, {440.0});
    tng::Deck deck;
    deck.setTrack(std::move(track));
    REQUIRE(deck.setLoop(static_cast<int64_t>(1.0 * fs), static_cast<int64_t>(3.0 * fs)));
    deck.setLoopActive(true);
    deck.setPlaying(true);

    const int64_t fs64 = fs;
    auto out = renderFrames(deck, fs64 / 2);  // transport fade done (pos 0.5 s)
    auto out2 = renderFrames(deck, fs64 * 4); // crosses loop out at ~3 s

    // Crossfade worst-case step over the wrap: two 440 Hz paths blended over
    // 88 frames ~= 0.012 + sine derivative 0.031 => < 0.08 (a hard cut shows
    // up to the sine's full span, same threshold as the seek test above).
    CHECK(maxStep(out, 0, fs64 / 2) < 0.08f);
    CHECK(maxStep(out2, 0, fs64 * 4) < 0.08f);

    // It actually wrapped: position is back inside the loop, still playing.
    CHECK(deck.positionFrames() >= 1.0 * fs);
    CHECK(deck.positionFrames() < 3.0 * fs);
    CHECK(deck.playing());
}

TEST_CASE("loop: activation past loop out wraps with overshoot (SPEC 4.5)") {
    const uint32_t fs = 8000;
    tng::Deck deck;
    deck.setTrack(makeSineTrack(fs, 10.0, {440.0}));
    REQUIRE(deck.setLoop(1000, 3000));
    deck.setPlaying(true);
    renderFrames(deck, 500); // transport fade-in done

    deck.requestSeek(9000); // way past the loop out
    deck.setLoopActive(true);
    renderFrames(deck, 100);

    // 9000 - 3000 = 6000, fmod by the loop length 2000 -> 0 => lands at in.
    CHECK(deck.positionFrames() >= 1000.0);
    CHECK(deck.positionFrames() < 3000.0);
}

TEST_CASE("loop: inactive loop never wraps; toggling starts/stops wrapping") {
    const uint32_t fs = 8000;
    tng::Deck deck;
    deck.setTrack(makeSineTrack(fs, 10.0, {440.0})); // 80000 frames
    REQUIRE(deck.setLoop(2000, 4000));
    deck.setPlaying(true);

    renderFrames(deck, 6000); // inactive: runs straight past the loop out
    CHECK(deck.positionFrames() >= 4000.0);

    deck.setLoopActive(true);
    renderFrames(deck, 3000); // wraps back inside the loop
    CHECK(deck.positionFrames() >= 2000.0);
    CHECK(deck.positionFrames() < 4000.0);

    deck.setLoopActive(false);
    renderFrames(deck, 3000); // off again: leaves the loop region
    CHECK(deck.positionFrames() >= 4000.0);
}

TEST_CASE("loop: loop reaching the track end keeps playing (SPEC 4.5)") {
    const uint32_t fs = 8000;
    tng::Deck deck;
    deck.setTrack(makeSineTrack(fs, 2.0, {440.0})); // 16000 frames
    REQUIRE(deck.setLoop(12000, 16000));
    deck.setLoopActive(true);
    deck.setPlaying(true);

    renderFrames(deck, 20000); // would end the track without the loop

    CHECK(deck.playing());
    CHECK(deck.positionFrames() >= 12000.0);
    CHECK(deck.positionFrames() < 16000.0);
}

TEST_CASE("loop: hot publish and setTrack clear the points (SPEC 4.7)") {
    const uint32_t fs = 8000;
    tng::Deck deck;
    deck.setTrack(makeSineTrack(fs, 2.0, {440.0}));
    REQUIRE(deck.setLoop(500, 1000));
    deck.setLoopActive(true);

    deck.publishTrack(makeSineTrack(fs, 3.0, {880.0}));
    CHECK(deck.loopIn() == -1);
    CHECK(deck.loopOut() == -1);
    CHECK_FALSE(deck.loopActive());

    REQUIRE(deck.setLoop(700, 1500));
    deck.setLoopActive(true);
    deck.setTrack(makeSineTrack(fs, 4.0, {220.0}));
    CHECK(deck.loopIn() == -1);
    CHECK(deck.loopOut() == -1);
    CHECK_FALSE(deck.loopActive());
}
