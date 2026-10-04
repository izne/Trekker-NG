#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdio>
#include <cstdlib>
#include <new>

// ---------------------------------------------------------------------------
// SPEC §9.6 realtime-safety hook.
//
// Replaces the global allocation functions for this test binary and counts
// every heap allocation made while a test holds an rtcheck::Scope. The
// engine's audio path (Deck::render, Mixer::process) must allocate nothing -
// SPEC §4.7 forbids it in the callback.
//
// Only the `new` side is replaced: counting needs it, and libstdc++'s
// default operator delete calls free(), which pairs with the malloc() below
// on MinGW (replacing delete too only upsets -Wmismatched-new-delete).
//
// The engine is compiled into this executable, so its operator new calls
// bind to the definitions below. Allocations inside libstdc++'s own DLL
// internals (e.g. exception formatting) are out of scope by design - the
// render path never throws.
// ---------------------------------------------------------------------------

namespace rtcheck {
thread_local int inScope = 0;
thread_local long allocCount = 0;
} // namespace rtcheck

namespace {
// Unbuffered stdout so a crash mid-test still leaves the assertions printed
// so far in the output (doctest buffers would be lost otherwise).
struct UnbufferedStdout {
    UnbufferedStdout() { std::setvbuf(stdout, nullptr, _IONBF, 0); }
} g_unbufferedStdout;
} // namespace

void* operator new(std::size_t n) {
    if (rtcheck::inScope > 0) ++rtcheck::allocCount;
    void* p = std::malloc(n ? n : 1);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (rtcheck::inScope > 0) ++rtcheck::allocCount;
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}
