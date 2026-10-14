// CueRing tests (M10, SPEC §4.8): the SPSC stereo ring that bridges the
// master and cue output devices - round trip, whole-block drop on overrun,
// zero-fill on underrun, and the setup-time edge cases. Concurrency itself
// is covered by construction (single producer, single consumer); here both
// sides are driven sequentially.

#include "doctest.h"

#include "cue_ring.h"

#include <vector>

namespace {

std::vector<float> pattern(int frames) {
    std::vector<float> v(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames * 2; ++i) {
        v[static_cast<size_t>(i)] = 0.1f * static_cast<float>(i + 1);
    }
    return v;
}

} // namespace

TEST_CASE("cue ring: round trip") {
    tng::CueRing ring;
    ring.resize(16);
    CHECK(ring.capacityFrames() == 16);

    const std::vector<float> src = pattern(12);
    REQUIRE(ring.push(src.data(), 12) == 12);
    CHECK(ring.dropped() == 0);

    std::vector<float> dst(12 * 2, -1.0f);
    ring.pop(dst.data(), 12);
    CHECK(dst == src); // sample-exact in both channels
    CHECK(ring.underrun() == 0);
}

TEST_CASE("cue ring: overrun drops the whole block") {
    tng::CueRing ring;
    ring.resize(8);

    // 12 frames into 8 free: nothing fits, so the block goes down whole -
    // never a partial write (the consumer would see torn audio).
    const std::vector<float> big = pattern(12);
    CHECK(ring.push(big.data(), 12) == 0);
    CHECK(ring.dropped() == 12);

    // The ring still works after the drop: an exact-size block fits.
    const std::vector<float> exact = pattern(8);
    CHECK(ring.push(exact.data(), 8) == 8);
    CHECK(ring.dropped() == 12); // unchanged

    std::vector<float> dst(8 * 2, -1.0f);
    ring.pop(dst.data(), 8);
    CHECK(dst == exact);
}

TEST_CASE("cue ring: underrun zero-fills") {
    tng::CueRing ring;
    ring.resize(8);

    const std::vector<float> src = pattern(4);
    REQUIRE(ring.push(src.data(), 4) == 4);

    std::vector<float> dst(8 * 2, -1.0f);
    ring.pop(dst.data(), 8); // ask for more than is buffered
    CHECK(dst[0] == src[0]); // available frames arrive intact ...
    CHECK(dst[7] == src[7]);
    for (size_t i = 8; i < dst.size(); ++i) {
        CHECK(dst[i] == 0.0f); // ... the rest is silence, not garbage
    }
    CHECK(ring.underrun() == 4);

    // An empty ring pops all-silence again and keeps counting.
    std::vector<float> empty(6 * 2, -1.0f);
    ring.pop(empty.data(), 6);
    for (float v : empty) CHECK(v == 0.0f);
    CHECK(ring.underrun() == 10);
}

TEST_CASE("cue ring: wrap-around order is preserved") {
    tng::CueRing ring;
    ring.resize(5); // deliberately not a power of two

    std::vector<float> got(5 * 2, -1.0f);
    const std::vector<float> first = pattern(3);
    REQUIRE(ring.push(first.data(), 3) == 3);
    ring.pop(got.data(), 2); // consume 2 of the 3 -> 1 frame left
    CHECK(got[0] == first[0]);
    CHECK(got[3] == first[3]);

    // Refill past the buffer end (write index wraps at 5).
    std::vector<float> second(4 * 2);
    for (int i = 0; i < 8; ++i) second[static_cast<size_t>(i)] = 1.0f + 0.1f * static_cast<float>(i);
    REQUIRE(ring.push(second.data(), 4) == 4); // free = 5 - 1 = 4: fits

    std::vector<float> out(5 * 2, -1.0f);
    ring.pop(out.data(), 5);
    CHECK(out[0] == first[4]); // frame 0 = the one carried-over frame ...
    CHECK(out[1] == first[5]);
    for (int i = 1; i < 5; ++i) { // ... frames 1..4 = the refilled block
        CHECK(out[static_cast<size_t>(i) * 2] == second[static_cast<size_t>(i - 1) * 2]);
        CHECK(out[static_cast<size_t>(i) * 2 + 1] ==
              second[static_cast<size_t>(i - 1) * 2 + 1]);
    }
    CHECK(ring.underrun() == 0);
    CHECK(ring.dropped() == 0);
}

TEST_CASE("cue ring: capacity 0 drops and zero-fills") {
    tng::CueRing ring;
    ring.resize(0);
    CHECK(ring.capacityFrames() == 0);

    const std::vector<float> src = pattern(4);
    CHECK(ring.push(src.data(), 4) == 0); // nothing to write into
    CHECK(ring.dropped() == 4);

    std::vector<float> dst(4 * 2, -1.0f);
    ring.pop(dst.data(), 4);
    for (float v : dst) CHECK(v == 0.0f);
    CHECK(ring.underrun() == 4);
}

TEST_CASE("cue ring: null and zero-length operations are safe") {
    tng::CueRing ring;
    ring.resize(8);

    const std::vector<float> src = pattern(4);
    CHECK(ring.push(src.data(), 0) == 0); // zero-length: nothing to count
    CHECK(ring.dropped() == 0);
    CHECK(ring.push(nullptr, 4) == 0);
    CHECK(ring.dropped() == 4); // counted: frames the producer lost

    std::vector<float> dst(4 * 2, -1.0f);
    ring.pop(nullptr, 4); // no crash, no output
    ring.pop(dst.data(), 0); // zero-length: no underrun counted
    CHECK(ring.underrun() == 0);
    for (float v : dst) CHECK(v == -1.0f); // untouched
}

TEST_CASE("cue ring: clear resets indices and counters") {
    tng::CueRing ring;
    ring.resize(8);

    const std::vector<float> src = pattern(8);
    REQUIRE(ring.push(src.data(), 8) == 8);
    std::vector<float> junk(8 * 2, 0.0f);
    ring.pop(junk.data(), 8);
    ring.push(junk.data(), 8); // dropped is0 so far...
    CHECK(ring.push(src.data(), 1) == 0); // ...now full: 1 dropped
    CHECK(ring.dropped() == 1);
    ring.pop(junk.data(), 8);
    ring.pop(junk.data(), 4); // 4 zero frames -> underrun
    CHECK(ring.underrun() == 4);

    ring.clear();
    CHECK(ring.dropped() == 0);
    CHECK(ring.underrun() == 0);
    // Storage kept (capacity unchanged) and the ring is usable again.
    CHECK(ring.capacityFrames() == 8);
    REQUIRE(ring.push(src.data(), 8) == 8);
    std::vector<float> dst(8 * 2, -1.0f);
    ring.pop(dst.data(), 8);
    CHECK(dst == src);
}
