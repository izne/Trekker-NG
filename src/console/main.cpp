// Trekker-NG console front-end (pitch PoC, M1-M2).
//
// Loads one track (folder or .zip with meta.json + 4 stems), plays it
// through miniaudio with a shared playhead per SPEC §4.2, and offers
// per-stem mutes plus Technics-style varispeed pitch via the keyboard.

#include "audio_device.h"
#include "deck.h"
#include "mixer.h"
#include "track_loader.h"

#include "miniaudio.h" // ma_encoder for --render (declarations only)

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#error "M1 console front-end is Windows-only; the engine stays portable (Linux UI comes later)."
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// Pitch: fixed ±10% range for M1 (SPEC §4.3 default), 0.10% steps from the
// keyboard, 0.01% steps with Shift (user request - finer than SPEC's minimum).
constexpr double kMaxPitchPct = 10.0;
constexpr double kStepCoarsePct = 0.10;
constexpr double kStepFinePct = 0.01;
constexpr uint32_t kPeriodFrames = 256; // SPEC §4.7 default

struct Options {
    std::string track;
    bool render = false;
    std::string renderOut;
    double rate = 1.0;
    double seconds = 5.0;
    std::string stems = "1111"; // for --render only
};

std::atomic<bool> g_quit{false};

#ifdef _WIN32
BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_quit.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}
#endif

void printUsage() {
    std::printf(
        "Trekker-NG M2 (console)\n"
        "\n"
        "usage:\n"
        "  trekker-ng <track.zip | track-folder> [options]\n"
        "\n"
        "options:\n"
        "  --render <out.wav>  render offline to a WAV instead of playing (no soundcard)\n"
        "  --rate <x>          playback rate for --render (e.g. 1.10 = +10%%)\n"
        "  --seconds <n>       seconds to render (default 5)\n"
        "  --stems <bbbb>      4-bit stem mask to keep on while rendering (default 1111)\n"
        "\n"
        "keys (live mode):\n"
        "  Space        play / pause\n"
        "  1 2 3 4      toggle stem  (Shift+digit: solo; US layout !@#$ also solos)\n"
        "  A            all stems on\n"
        "  = + ]        pitch up   0.10%%   (hold Shift: 0.01%%)\n"
        "  - _ [        pitch down 0.10%%   (hold Shift: 0.01%%)\n"
        "  0            reset pitch to 0.00%%\n"
        "  I            toggle interpolator: cubic Hermite / linear (debug)\n"
        "  Q / ESC      quit\n");
}

bool parseArgs(int argc, char** argv, Options& opt, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                err = std::string(name) + " needs a value";
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--render") {
            const char* v = next("--render");
            if (!v) return false;
            opt.render = true;
            opt.renderOut = v;
        } else if (a == "--rate") {
            const char* v = next("--rate");
            if (!v) return false;
            opt.rate = std::atof(v);
            if (opt.rate <= 0.0) {
                err = "--rate must be > 0";
                return false;
            }
        } else if (a == "--seconds") {
            const char* v = next("--seconds");
            if (!v) return false;
            opt.seconds = std::atof(v);
            if (opt.seconds <= 0.0) {
                err = "--seconds must be > 0";
                return false;
            }
        } else if (a == "--stems") {
            const char* v = next("--stems");
            if (!v) return false;
            opt.stems = v;
            if (opt.stems.size() != 4 ||
                opt.stems.find_first_not_of("01") != std::string::npos) {
                err = "--stems must be 4 bits, e.g. 1011";
                return false;
            }
        } else if (a == "--help" || a == "-h") {
            err.clear();
            return false;
        } else if (!a.empty() && a[0] == '-') {
            err = "unknown option: " + a;
            return false;
        } else if (opt.track.empty()) {
            opt.track = a;
        } else {
            err = "unexpected extra argument: " + a;
            return false;
        }
    }
    if (opt.track.empty()) {
        err = "missing track path (folder or .zip)";
        return false;
    }
    return true;
}

std::string truncateUtf8Safe(const std::string& s, size_t maxChars) {
    // Byte-level truncation is fine for the status line; multi-byte chars may
    // be cut at the boundary only in pathological titles.
    if (s.size() <= maxChars) return s;
    return s.substr(0, maxChars) + "..";
}

std::string formatPitch(double rate) {
    char buf[32];
    const double pct = (rate - 1.0) * 100.0;
    std::snprintf(buf, sizeof(buf), "%s%.2f%%", pct >= 0 ? "+" : "", pct);
    return buf;
}

int runRender(const Options& opt, tng::Deck& deck, const tng::DeckData& data) {
    tng::Mixer mixer; // same master gain + clamp as the live path
    deck.setRate(opt.rate);
    deck.setPlaying(true);

    ma_encoder_config cfg =
        ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 2, data.sampleRate);
    ma_encoder encoder;
    const std::wstring wideOut = std::filesystem::path(opt.renderOut).wstring();
    if (ma_encoder_init_file_w(wideOut.c_str(), &cfg, &encoder) != MA_SUCCESS) {
        std::fprintf(stderr, "error: cannot open output file: %s\n", opt.renderOut.c_str());
        return 1;
    }

    const uint64_t totalFrames =
        static_cast<uint64_t>(opt.seconds * static_cast<double>(data.sampleRate) + 0.5);
    std::vector<float> buf(static_cast<size_t>(kPeriodFrames) * 2);
    std::vector<int16_t> pcm(static_cast<size_t>(kPeriodFrames) * 2);
    uint64_t written = 0;
    while (written < totalFrames && deck.playing()) {
        deck.render(buf.data(), kPeriodFrames);
        mixer.process(buf.data(), nullptr, buf.data(), kPeriodFrames);
        const ma_uint64 want = std::min<uint64_t>(kPeriodFrames, totalFrames - written);
        // ma_encoder does not convert formats - hand it exactly what the
        // config promised (s16), clamped like the live output stage does.
        for (ma_uint64 i = 0; i < want * 2; ++i) {
            float v = buf[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pcm[i] = static_cast<int16_t>(v * 32767.0f);
        }
        ma_uint64 got = 0;
        ma_encoder_write_pcm_frames(&encoder, pcm.data(), want, &got);
        if (got == 0) break;
        written += got;
    }
    ma_encoder_uninit(&encoder);

    const double secs = static_cast<double>(written) / data.sampleRate;
    std::printf("rendered %llu frames (%.2f s) at rate %.4f -> %s\n",
                static_cast<unsigned long long>(written), secs, opt.rate,
                opt.renderOut.c_str());
    if (written < totalFrames) {
        std::printf("note: deck reached the end of the track before the requested length\n");
    }
    return 0;
}

int runLive(const Options& opt, tng::Deck& deck, const tng::DeckData& data) {
    tng::Mixer mixer;
    tng::AudioDevice device;
    std::string err;
    if (!device.init(&deck, nullptr, &mixer, data.sampleRate, kPeriodFrames, nullptr, &err)) {
        std::fprintf(stderr, "error: audio device init failed: %s\n", err.c_str());
        return 1;
    }
    if (!device.start(&err)) {
        std::fprintf(stderr, "error: audio device start failed: %s\n", err.c_str());
        device.shutdown();
        return 1;
    }
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    std::printf(
        "\n"
        "loaded: %s%s%s | %u Hz | %d stems\n"
        "keys: Space play/pause | 1-4 stem toggle (Shift=solo) | A all on\n"
        "      =+] pitch up | -_[ pitch down (Shift = 0.01%%) | 0 reset | I interp | Q quit\n"
        "\n",
        data.title.c_str(), data.artist.empty() ? "" : " - ", data.artist.c_str(),
        data.sampleRate, tng::Deck::kStemCount);

    int soloIdx = -1;
    bool dirty = true;
    DWORD lastDraw = 0;
    size_t prevLineLen = 0;
    std::string line;

    auto pitchBy = [&](int dir) {
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const double step = shift ? kStepFinePct : kStepCoarsePct;
        double pct = (deck.rate() - 1.0) * 100.0 + static_cast<double>(dir) * step;
        pct = std::round(pct * 100.0) / 100.0; // snap to the 0.01% grid
        pct = std::max(-kMaxPitchPct, std::min(kMaxPitchPct, pct));
        deck.setRate(1.0 + pct / 100.0);
        dirty = true;
    };

    auto doSolo = [&](int idx) {
        if (soloIdx == idx) {
            deck.setAllStems(true);
            soloIdx = -1;
        } else {
            deck.soloStem(idx);
            soloIdx = idx;
        }
        dirty = true;
    };

    while (!g_quit.load(std::memory_order_relaxed)) {
        while (_kbhit()) {
            const int c = _getch();
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            switch (c) {
                case ' ': {
                    if (!deck.playing() &&
                        deck.positionFrames() >= static_cast<double>(data.frames) - 1.0) {
                        deck.requestSeek(0); // restart from the end
                    }
                    deck.setPlaying(!deck.playing());
                    soloIdx = -1;
                    dirty = true;
                    break;
                }
                case '1': case '2': case '3': case '4': {
                    const int idx = c - '1';
                    if (shift) {
                        doSolo(idx);
                    } else {
                        deck.toggleStem(idx);
                        soloIdx = -1;
                        dirty = true;
                    }
                    break;
                }
                case '!': case '@': case '#': case '$': { // US-layout Shift+1..4
                    const int idx = std::string("!@#$").find(c);
                    if (idx != std::string::npos) doSolo(idx);
                    break;
                }
                case 'a': case 'A':
                    deck.setAllStems(true);
                    soloIdx = -1;
                    dirty = true;
                    break;
                case '=': case '+': case ']': case '}': pitchBy(+1); break;
                case '-': case '_': case '[': case '{': pitchBy(-1); break;
                case '0':
                    deck.setRate(1.0);
                    dirty = true;
                    break;
                case 'i': case 'I': // debug: toggle cubic Hermite <-> linear (SPEC §4.3)
                    deck.setInterp(deck.interp() == tng::InterpMode::Hermite
                                       ? tng::InterpMode::Linear
                                       : tng::InterpMode::Hermite);
                    dirty = true;
                    break;
                case 27: // ESC
                case 'q': case 'Q':
                    g_quit.store(true, std::memory_order_relaxed);
                    break;
                default: break;
            }
        }

        const DWORD now = GetTickCount();
        if (dirty || now - lastDraw >= 200) {
            char head[16];
            std::snprintf(head, sizeof(head), "[%s]", deck.playing() ? "PLAY" : "PAUSE");

            char bpmText[48] = "";
            if (data.bpm > 0.0f) {
                std::snprintf(bpmText, sizeof(bpmText), " (%.1f BPM)",
                              static_cast<double>(data.bpm) * deck.rate());
            }

            char stems[48];
            std::snprintf(stems, sizeof(stems), "%d:%s %d:%s %d:%s %d:%s", 1,
                          deck.stemOn(0) ? "on" : "--", 2, deck.stemOn(1) ? "on" : "--", 3,
                          deck.stemOn(2) ? "on" : "--", 4, deck.stemOn(3) ? "on" : "--");

            const double posSec =
                deck.positionFrames() / static_cast<double>(data.sampleRate);
            const double totalSec =
                static_cast<double>(data.frames) / static_cast<double>(data.sampleRate);

            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s %s | %s%s | %6.2f/%.1fs | %s", head,
                          truncateUtf8Safe(data.title, 28).c_str(),
                          formatPitch(deck.rate()).c_str(), bpmText, posSec, totalSec, stems);

            line = buf;
            std::printf("\r%s%s", line.c_str(),
                        line.size() < prevLineLen ? std::string(prevLineLen - line.size(), ' ').c_str()
                                                  : "");
            std::fflush(stdout);
            prevLineLen = line.size();
            lastDraw = now;
            dirty = false;
        }
        Sleep(5);
    }

    device.stop();
    device.shutdown();
    SetConsoleCtrlHandler(consoleHandler, FALSE);
    std::printf("\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    Options opt;
    std::string err;
    if (!parseArgs(argc, argv, opt, err)) {
        if (!err.empty()) std::fprintf(stderr, "error: %s\n\n", err.c_str());
        printUsage();
        return err.empty() ? 0 : 1;
    }

    tng::LoadResult loaded = tng::loadTrack(opt.track);
    if (!loaded.ok()) {
        std::fprintf(stderr, "error: %s\n", loaded.error.c_str());
        return 1;
    }
    const tng::DeckData& data = *loaded.data;

    tng::Deck deck;
    if (opt.render) {
        // Apply the stem mask before setTrack() so gains start at the target
        // values (no fade-in at render start).
        for (int i = 0; i < tng::Deck::kStemCount; ++i) {
            deck.setStem(i, opt.stems[static_cast<size_t>(i)] == '1');
        }
    }
    deck.setTrack(std::move(loaded.data));

    std::printf("track: %s", data.title.c_str());
    if (!data.artist.empty()) std::printf(" - %s", data.artist.c_str());
    std::printf(" | %.1f BPM | %u Hz | %.1fs\n", data.bpm, data.sampleRate,
                static_cast<double>(data.frames) / data.sampleRate);
    for (int i = 0; i < tng::Deck::kStemCount; ++i) {
        const tng::Stem& s = data.stems[static_cast<size_t>(i)];
        std::printf("  stem %d: %-12s %s\n", i + 1, s.frames > 0 ? s.name.c_str() : "(silent)",
                    s.frames > 0 ? "ok" : "-");
    }

    if (opt.render) {
        return runRender(opt, deck, data);
    }
    return runLive(opt, deck, data);
}
