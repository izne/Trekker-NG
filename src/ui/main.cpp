// Trekker-NG M4c UI: SDL2 window + OpenGL 3 + Dear ImGui, two decks with a
// mixer column in between (SPEC §6), plus the Single/Mix mode toggle.
//
// Track loading runs on a worker thread (SPEC §4.7: prepared off-thread),
// then publishTrack() swaps it in atomically; the audio callback adopts it on
// the next block. Dropping a folder or .zip loads the deck under the drop
// position (SPEC §6.156).

#include "deck_view.h"

#include "audio_device.h"
#include "deck.h"
#include "mixer.h"
#include "settings.h"
#include "track_loader.h"

#include "SDL.h"
#include "SDL_opengl.h"
#include "SDL_syswm.h" // M5e: HWND of our window for the Load... dialog owner
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include "font_dseg.inc" // embedded DSEG7 Classic (SIL OFL 1.1), M4display

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr int kDeckCount = 2;
constexpr float kMixerWidth = 150.0f; // center column in Mix mode
constexpr float kStatusReserve = 82.0f; // bottom lines under the deck children (2 hint lines)
constexpr const char* kXfNames[3] = {"lin", "pw", "cut"}; // crossfader curves (M5b/M5c)

void showError(const char* what) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Trekker-NG", what, nullptr);
}

const char* deckName(int i) { return i == 0 ? "A" : "B"; }

} // namespace

ImFont* tui::vfdFont = nullptr; // set right after CreateContext (M4display)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    // Console subsystem (keeps stderr for debugging) but the app is graphical.
    if (HWND console = GetConsoleWindow()) ShowWindow(console, SW_HIDE);
#endif

    SDL_SetMainReady(); // SDL_MAIN_HANDLED: we own main (see CMakeLists)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        showError(SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // M5c: user settings, loaded from trekker-ng.json next to the exe
    // (portable-style, same nlohmann JSON stack as meta.json / cue files).
    // Missing or corrupt file = defaults (Settings::load keeps them).
    // M5f: loaded before the window exists - the saved windowed size and
    // fullscreen flag seed SDL_CreateWindow below.
    tng::Settings settings;
    std::filesystem::path settingsPath;
    {
        char* base = SDL_GetBasePath();
        settingsPath = std::filesystem::u8path(base ? base : "./");
        SDL_free(base);
        settingsPath /= "trekker-ng.json";
        settings.load(settingsPath);
    }

    SDL_Window* window = SDL_CreateWindow(
        "Trekker-NG", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        settings.windowWidth, settings.windowHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        showError(SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext glctx = SDL_GL_CreateContext(window);
    if (!glctx) {
        showError(SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    // M5f: restore borderless fullscreen from the previous session (the
    // windowed size above stays the restore target inside SDL).
    if (settings.fullscreen)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // M4display: fonts. [0] stays the stock default so every widget looks
    // exactly like M4c; [1] is DSEG7 Classic (embedded, SIL OFL 1.1) for the
    // VFD readouts, with the default font merged into it so the glyphs DSEG7
    // lacks ('+', '%', '/') fall back automatically. First source wins.
    // '.' is excluded from DSEG7 on purpose: its period has zero width and
    // disappears under the next digit ('1.4' read as '14'); the default
    // font's period takes over instead.
    io.Fonts->AddFontDefault();
    ImFontConfig dsegCfg;
    dsegCfg.FontDataOwnedByAtlas = false; // static array, must outlive the atlas
    static const ImWchar kDsegExclude[] = {0x002E, 0x002E, 0};
    dsegCfg.GlyphExcludeRanges = kDsegExclude;
    tui::vfdFont = io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(kDseg7ClassicTtf),
        static_cast<int>(sizeof(kDseg7ClassicTtf)), 15.0f, &dsegCfg);
    ImFontConfig mergeCfg;
    mergeCfg.MergeMode = true;
    io.Fonts->AddFontDefault(&mergeCfg);

    // No ImGui keyboard nav: global shortcuts (Space, 1-4) must never fire
    // twice - once through nav activation, once through SDL_KEYDOWN.
    ImGui_ImplSDL2_InitForOpenGL(window, glctx);
    ImGui_ImplOpenGL3_Init("#version 130");

    bool showSettings = false;
    std::vector<std::string> deviceNames; // filled lazily by the settings window
    bool deviceNamesLoaded = false;
    uint32_t periodFrames = static_cast<uint32_t>(settings.bufferFrames); // audio callback size

    std::array<tng::Deck, kDeckCount> decks;
    std::array<tui::DeckView, kDeckCount> views;
    tng::Mixer mixer;
    tng::AudioDevice device;
    bool deviceUp = false;
    uint32_t devRate = 0;

#ifdef _WIN32
    // M5e: the HWND both deck views use to parent their Load... zip dialog.
    {
        SDL_SysWMinfo wmi;
        SDL_VERSION(&wmi.version);
        if (SDL_GetWindowWMInfo(window, &wmi)) {
            for (tui::DeckView& v : views) v.setNativeHwnd(wmi.info.win.window);
        }
    }
#endif

    // M5c: the boot mode is a persisted setting (the M toggle writes it
    // back on every flip, so the next boot starts where you left off).
    bool mixMode = settings.bootMixMode;
    int activeDeck = 0;   // deck the global keys target (hovered panel in Mix)

    // UI-session memory: the EQ level a kill button had before it was
    // pressed, so clicking a lit kill restores that level instead of flat
    // (user decision 2026-10-05). UI thread only; default 0.5 = flat for
    // the slider-dragged-to-0 edge case (no saved level yet).
    float eqPreKill[kDeckCount][tng::Eq3::kBands] = {
        {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}};

    // M5b UI-session state: frames left to show the mixer column's CLIP
    // light after the master soft clip engages (refreshed every frame from
    // the mixer's one-shot flag; ~0.5 s at 60 fps).
    int uiClipFrames = 0;

    // M5g: smoothed VU levels for the mixer-column meters (Mix mode):
    // same peak-hold + per-frame fall the deck views use for their bars.
    float vuLevel[kDeckCount] = {0.0f, 0.0f};

    // Single mode: deck A through the crossfader at exactly gain 1.0
    // (crossfader -1) -> loudness identical to M3; Mix: centered, both
    // decks audible. Deck B is registered with the device in both modes (a
    // paused deck renders near-free silence) so toggling never re-inits.
    // M5c: also seeds the persisted mixer/deck-view settings.
    mixer.setCrossfader(mixMode ? 0.0f : -1.0f);
    mixer.setXfCurve(settings.xfCurve);
    mixer.setMasterGain(settings.masterGain);
    for (int i = 0; i < kDeckCount; ++i) {
        views[i].setPitchRange(decks[i], settings.pitchRangePct);
        views[i].setPitchReversed(settings.pitchReversed);
        views[i].setReadoutSize(settings.vfdTimePitchSize); // M5e
    }

    std::string status = "drop a track folder or .zip, or use Load...";
    bool statusError = false;
    std::array<std::optional<std::future<tng::LoadResult>>, kDeckCount> loading;
    // M4d: the path each deck is currently loading/loaded from - threaded to
    // the deck view on success so hot-cue edits can saveCues() through it.
    std::array<std::filesystem::path, kDeckCount> pendingPath;

    auto setStatus = [&](const std::string& s, bool err) {
        status = s;
        statusError = err;
        std::fprintf(stderr, "[%s] %s\n", err ? "error" : "info", s.c_str());
        std::fflush(stderr);
    };

    // M5c: write-through persistence - every settings change saves
    // immediately (same immediate semantics as the hot-cue sidecars).
    auto saveSettings = [&] {
        if (!settings.save(settingsPath)) {
            setStatus("settings: could not write " + settingsPath.u8string(), true);
        }
    };

    // Pitch from the keyboard, same feel as the console: 0.10% steps,
    // Shift = 0.01% fine, snapped to the 0.01% grid, clamped to the active
    // deck's range (M4d: 10/16 toggle). Acts on the active deck (hover a
    // panel to switch in Mix mode) and backs off while a nudge owns the rate.
    auto pitchBy = [&](int dir) {
        tui::DeckView& view = views[activeDeck];
        if (view.nudging()) return; // the nudge restores its base on release
        tng::Deck& d = decks[activeDeck];
        const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        const double step = shift ? 0.01 : 0.10;
        double pct = std::round(((d.rate() - 1.0) * 100.0 +
                                 static_cast<double>(dir) * step) *
                                100.0) /
                     100.0;
        const double range = static_cast<double>(view.pitchRange());
        pct = std::max(-range, std::min(range, pct));
        d.setRate(1.0 + pct / 100.0);
    };

    // M4d: jump the active deck to a hot cue (Shift+1..8 / cue buttons).
    auto hotCueTrigger = [&](int di, int slot) {
        const tng::DeckData* td = decks[di].track();
        if (!td || slot < 0 || slot >= static_cast<int>(td->cues.size())) return;
        const double fs = static_cast<double>(std::max(td->sampleRate, 1u));
        int64_t frame =
            static_cast<int64_t>(td->cues[static_cast<size_t>(slot)].positionMs * fs / 1000.0);
        if (frame < 0) frame = 0;
        if (frame >= td->frames) frame = td->frames - 1;
        decks[di].requestSeek(frame);
    };

    // M4d: quick loop on the active deck (Alt+1..5).
    auto quickLoop = [&](int beats) {
        tng::Deck& d = decks[activeDeck];
        const tng::DeckData* td = d.track();
        if (!td || td->bpm <= 0.0f) {
            setStatus("quick loop: track has no BPM in meta.json", true);
            return;
        }
        if (!d.setQuickLoop(beats)) setStatus("quick loop: not possible here", true);
    };

    auto startDevice = [&](uint32_t rate) -> bool {
        std::string err;
        mixer.setSampleRate(rate); // M5a: EQ smoothing runs in real time
        // M5c: named device from settings ("" = system default).
        const char* devName =
            settings.deviceName.empty() ? nullptr : settings.deviceName.c_str();
        if (!device.init(&decks[0], &decks[1], &mixer, rate, periodFrames, devName, &err)) {
            setStatus("audio init failed: " + err, true);
            return false;
        }
        if (!device.start(&err)) {
            device.shutdown();
            setStatus("audio start failed: " + err, true);
            return false;
        }
        deviceUp = true;
        devRate = rate;
        return true;
    };

    // M5c: device/buffer changes re-init the device at the current rate
    // (brief dropout, same as the sample-rate restart in onLoaded).
    auto applyDevice = [&]() {
        if (deviceUp && devRate != 0) {
            device.stop();
            device.shutdown();
            deviceUp = false;
            startDevice(devRate);
        }
    };

    auto beginLoad = [&](int di, const std::filesystem::path& path) {
        if (loading[di]) {
            setStatus(std::string("deck ") + deckName(di) +
                          ": still loading - wait for the current track",
                      true);
            return;
        }
        pendingPath[di] = path;
        loading[di] = std::async(std::launch::async, [path] { return tng::loadTrack(path); });
        setStatus(std::string("loading deck ") + deckName(di) + "...", false);
    };

    auto onLoaded = [&](int di, tng::LoadResult res) {
        if (!res.ok()) {
            setStatus(std::string("deck ") + deckName(di) + " error: " + res.error, true);
            return;
        }
        const uint32_t rate = res.data->sampleRate;

        // Sample-rate rule (v1, no SRC - SPEC §3 has no resampler): all
        // loaded tracks must share the device rate. A mismatch with the
        // *other* deck is refused with an explanation; with an empty other
        // deck the device is simply restarted at the new rate.
        const tng::DeckData* other = decks[1 - di].track();
        if (other != nullptr && other->sampleRate != rate) {
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                          "deck %s: %u Hz track refused - deck %s runs at %u Hz "
                          "(v1 has no sample-rate conversion)",
                          deckName(di), rate, deckName(1 - di), other->sampleRate);
            setStatus(buf, true);
            return;
        }
        if (deviceUp && rate != devRate) {
            // Sample rate changed: restart the device at the track's rate.
            device.stop();
            device.shutdown();
            deviceUp = false;
        }

        decks[di].publishTrack(std::move(res.data)); // hot swap (SPEC §4.7)
        views[di].onTrackChanged();
        views[di].setTrackPath(pendingPath[di]); // M4d: hot-cue saves need it
        if (!deviceUp) startDevice(rate);

        const tng::DeckData* d = decks[di].track();
        char buf[320];
        std::snprintf(buf, sizeof(buf), "loaded %s: %s%s%s | %u Hz | %.1f s",
                      deckName(di), d->artist.c_str(),
                      d->artist.empty() ? "" : " - ", d->title.c_str(), d->sampleRate,
                      static_cast<double>(d->frames) / static_cast<double>(d->sampleRate));
        setStatus(buf, !deviceUp);
    };

    auto toggleMode = [&]() {
        mixMode = !mixMode;
        if (!mixMode) {
            decks[1].setPlaying(false); // deck B pauses entering Single (agreed)
            mixer.setCrossfader(-1.0f); // deck A at exactly gain 1.0 = M3 loudness
            activeDeck = 0;
        } else {
            mixer.setCrossfader(0.0f); // centered: both decks audible
        }
        settings.bootMixMode = mixMode; // M5c: next boot starts in the last mode
        saveSettings();
        setStatus(mixMode ? "mix mode - two decks, crossfader centered"
                          : "single mode - deck B paused",
                  false);
    };

    if (argc > 1) beginLoad(0, std::filesystem::path(argv[1]));
    if (argc > 2) beginLoad(1, std::filesystem::path(argv[2])); // deck B (Mix mode)

    bool running = true;
    while (running) {
        // SDL_TEXTINPUT carries the global pitch keys (+ = - _ [ ] 0) with the
        // right character for the active layout; ImGui turns text input off
        // when no field is focused, so re-enable it before polling.
        if (!io.WantTextInput) SDL_StartTextInput();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_DROPFILE) {
                if (e.drop.file) {
                    // SPEC §6.156: dropping loads the deck under the drop
                    // position; Single mode has only deck A visible. This
                    // SDL2 build's SDL_DropEvent carries no x/y, so route by
                    // the mouse position (still where the file was dropped).
                    int target = 0;
                    if (mixMode) {
                        int mx = 0, w = 0;
                        SDL_GetMouseState(&mx, nullptr);
                        SDL_GetWindowSize(window, &w, nullptr);
                        target = (mx * 2 < w) ? 0 : 1;
                    }
                    beginLoad(target, std::filesystem::u8path(e.drop.file));
                    SDL_free(e.drop.file);
                }
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat && !io.WantTextInput) {
                switch (e.key.keysym.sym) {
                    case SDLK_SPACE: {
                        tng::Deck& d = decks[activeDeck];
                        const tng::DeckData* td = d.track();
                        if (!d.playing() && td &&
                            d.positionFrames() >= static_cast<double>(td->frames) - 1.0) {
                            d.requestSeek(0); // restart from the end
                        }
                        d.setPlaying(!d.playing());
                        break;
                    }
                    // SPEC §6.155: 1-4 stems on deck A, 7-0 on deck B with
                    // plain digits only. M4d adds, on the active deck:
                    // Shift+1..8 trigger a hot cue, Alt+1..5 a quick loop.
                    // Shift+digit is a different character on some layouts
                    // (German "!§$"), so branches key off the modifier, not
                    // the produced character. Alt/Ctrl+digit never toggles
                    // stems (M4d: that used to leak through).
                    case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4:
                    case SDLK_5: case SDLK_6:
                    case SDLK_7: case SDLK_8: case SDLK_9: {
                        const SDL_Keycode sym = e.key.keysym.sym;
                        const Uint16 mod = e.key.keysym.mod;
                        if (mod & KMOD_SHIFT) {
                            const int slot = static_cast<int>(sym - SDLK_1);
                            if (slot <= 7) hotCueTrigger(activeDeck, slot);
                        } else if (mod & (KMOD_ALT | KMOD_CTRL | KMOD_GUI)) {
                            const int digit = static_cast<int>(sym - SDLK_1);
                            if (digit >= 0 && digit <= 4) quickLoop(1 << digit); // 1,2,4,8,16
                        } else if (sym <= SDLK_4) {
                            decks[0].toggleStem(sym - SDLK_1);
                        } else if (sym >= SDLK_7) {
                            decks[1].toggleStem(sym - SDLK_7);
                        }
                        break;
                    }
                    case SDLK_0:
                        // '0' is the pitch-reset key in Single mode (M3) and
                        // deck B's stem 4 in Mix mode (SPEC §6.155) - the
                        // TEXTINPUT handler below skips it while in Mix so one
                        // keypress never does both.
                        if (mixMode && !(e.key.keysym.mod & KMOD_SHIFT)) {
                            decks[1].toggleStem(3);
                        }
                        break;
                    // M4d loop keys on the active deck: I/O set the points,
                    // L toggles, Shift+L exits (clears) the loop.
                    case SDLK_i:
                        if (!(e.key.keysym.mod & (KMOD_SHIFT | KMOD_ALT | KMOD_CTRL))) {
                            decks[activeDeck].setLoopIn(
                                static_cast<int64_t>(decks[activeDeck].positionFrames()));
                        }
                        break;
                    case SDLK_o:
                        if (!(e.key.keysym.mod & (KMOD_SHIFT | KMOD_ALT | KMOD_CTRL))) {
                            decks[activeDeck].setLoopOut(
                                static_cast<int64_t>(decks[activeDeck].positionFrames()));
                        }
                        break;
                    case SDLK_l: {
                        tng::Deck& d = decks[activeDeck];
                        if (e.key.keysym.mod & KMOD_SHIFT) {
                            d.clearLoop();
                        } else if (!(e.key.keysym.mod & (KMOD_ALT | KMOD_CTRL))) {
                            d.setLoopActive(!d.loopActive()); // guard rejects bad pairs
                        }
                        break;
                    }
                    case SDLK_m:
                        toggleMode();
                        break;
                    case SDLK_q: case SDLK_ESCAPE:
                        running = false;
                        break;
                    case SDLK_F11: {
                        // M5f: borderless fullscreen toggle, persisted so
                        // the next boot restores the state.
                        const bool wasFs =
                            (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                        SDL_SetWindowFullscreen(
                            window, wasFs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                        settings.fullscreen = !wasFs;
                        saveSettings();
                        break;
                    }
                    default: break;
                }
            } else if (e.type == SDL_WINDOWEVENT &&
                       e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                // M4d: a nudge must not survive alt-tab (the mouse-up event
                // never arrives); restore the base pitch on both decks.
                for (int i = 0; i < kDeckCount; ++i) views[i].cancelNudge(decks[i]);
            } else if (e.type == SDL_WINDOWEVENT &&
                       e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                // M5f: remember the windowed size in memory (persisted at
                // exit / F11). Skip fullscreen and maximized - their sizes
                // must never overwrite the restore dimensions.
                const Uint32 fl = SDL_GetWindowFlags(window);
                if (!(fl & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
                    settings.windowWidth = e.window.data1;
                    settings.windowHeight = e.window.data2;
                }
            } else if (e.type == SDL_TEXTINPUT && !io.WantTextInput) {
                // Printable keys via their produced character, so every layout
                // works (US "=+-_[]0", German "+ -" keys, German Shift+0 "=").
                for (const char* c = e.text.text; *c != '\0'; ++c) {
                    switch (*c) {
                        case '+': case '=': case ']': pitchBy(+1); break;
                        case '-': case '_': case '[': pitchBy(-1); break;
                        case '0':
                            // M4d: skipped while a nudge owns the rate (the
                            // nudge would overwrite the reset anyway).
                            if (!mixMode && !views[activeDeck].nudging()) {
                                decks[activeDeck].setRate(1.0);
                            }
                            break;
                        default: break;
                    }
                }
            }
        }

        for (int i = 0; i < kDeckCount; ++i) {
            if (loading[i] &&
                loading[i]->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                onLoaded(i, loading[i]->get());
                loading[i].reset();
            }
            decks[i].drainRetired(); // release swapped-out data once adopted
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoScrollbar);

        const char* loadPath[kDeckCount] = {nullptr, nullptr};
        if (mixMode) {
            // Two panels side by side, mixer in the middle (SPEC §6.152).
            const float availW = ImGui::GetContentRegionAvail().x;
            const float availH =
                std::max(240.0f, ImGui::GetContentRegionAvail().y - kStatusReserve);
            const float panelW = std::max(240.0f, (availW - kMixerWidth) * 0.5f);
            const ImGuiWindowFlags childFlags =
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

            ImGui::BeginChild("deckA", ImVec2(panelW, availH), ImGuiChildFlags_None,
                              childFlags);
            if (const char* p = views[0].draw(decks[0], activeDeck == 0, false)) loadPath[0] = p;
            const ImVec2 a0 = ImGui::GetWindowPos();
            const ImVec2 a1(a0.x + ImGui::GetWindowSize().x, a0.y + ImGui::GetWindowSize().y);
            ImGui::EndChild();

            ImGui::SameLine();
            // --- mixer column: line faders, crossfader, master (SPEC §4.6) ---
            ImGui::BeginGroup();
            ImGui::TextDisabled("mixer");
            // M5e: A/B aligned explicitly. M5g: the deck VU meters moved
            // into this column - deck A's bar sits left of fader A, deck
            // B's right of fader B (each meter shows the deck its fader
            // controls), both at the fader row: same top, same height.
            // The A/B block therefore starts kVuW + gap in from the group
            // edge; everything below still starts at the group edge.
            constexpr float kLineSliderW = 38.0f;
            constexpr float kLineSliderH = 100.0f;
            constexpr float kVuW = 22.0f;
            const float vuGap = 4.0f;
            const float abGap = ImGui::GetStyle().ItemSpacing.x;
            const ImVec2 abTop = ImGui::GetCursorScreenPos(); // group left edge
            const float labelH = ImGui::GetTextLineHeightWithSpacing();
            const ImVec2 sliderTop(abTop.x + kVuW + vuGap, abTop.y + labelH);

            for (int d = 0; d < kDeckCount; ++d) {
                const float peak = std::min(1.0f, decks[d].blockPeak());
                vuLevel[d] = std::max(peak, vuLevel[d] * 0.88f);
                const float vx = (d == 0) ? abTop.x
                                          : sliderTop.x + kLineSliderW + abGap +
                                                kLineSliderW + vuGap;
                tui::drawVuBar(ImGui::GetWindowDrawList(), vx, sliderTop.y,
                               kVuW, kLineSliderH, vuLevel[d]);
            }

            ImGui::SetCursorScreenPos(ImVec2(sliderTop.x, abTop.y));
            ImGui::TextDisabled("A");
            ImGui::SetCursorScreenPos(sliderTop);
            float lineA = mixer.lineGain(0);
            ImGui::VSliderFloat("##lineA", ImVec2(kLineSliderW, kLineSliderH), &lineA,
                                0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemActive()) mixer.setLineGain(0, lineA);

            const ImVec2 bTop(sliderTop.x + kLineSliderW + abGap, abTop.y);
            ImGui::SetCursorScreenPos(bTop);
            ImGui::TextDisabled("B");
            ImGui::SetCursorScreenPos(ImVec2(bTop.x, sliderTop.y));
            float lineB = mixer.lineGain(1);
            ImGui::VSliderFloat("##lineB", ImVec2(kLineSliderW, kLineSliderH), &lineB,
                                0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemActive()) mixer.setLineGain(1, lineB);

            ImGui::SetCursorScreenPos(
                ImVec2(abTop.x, sliderTop.y + kLineSliderH + ImGui::GetStyle().ItemSpacing.y));

            // M5a: per-deck 3-band EQ (Mix mode only, SPEC §4.6). Traktor
            // feel - flat in the center, top = +6 dB boost, bottom = kill
            // (value semantics: 0.0 kill, 0.5 flat, 1.0 boost - Eq3::curve).
            // UI v2: three thin vertical sliders side by side (low/mid/high)
            // with an aligned label above each column and a kill toggle
            // square below it (lit while killed); right-click a slider to
            // snap it back to flat (double-click never worked: ImGui 1.92
            // sliders use it for inline text input). NOTE: SameLine(x)
            // takes an ABSOLUTE offset from the group start - a gap is
            // SameLine(0.0f, gap).
            ImGui::Spacing();
            ImGui::TextDisabled("eq");
            static const char* kBandLabel[3] = {"lo", "mid", "hi"}; // column tags
            static const char* kBandName[3] = {"low", "mid", "high"};
            constexpr float kSliderW = 16.0f;
            constexpr float kSliderH = 100.0f;
            constexpr float kGap = 8.0f; // label words need >=4 px between them
            constexpr float kKill = 16.0f;
            constexpr float kPitch = kSliderW + kGap; // column pitch
            for (int d = 0; d < kDeckCount; ++d) {
                ImGui::TextDisabled("%s", deckName(d));
                ImGui::SameLine();
                const float colX = ImGui::GetCursorPosX(); // first band column
                const ImVec2 labelPos = ImGui::GetCursorScreenPos();
                for (int b = 0; b < tng::Eq3::kBands; ++b) {
                    const float lw =
                        ImGui::CalcTextSize(kBandLabel[b]).x;
                    ImGui::SetCursorScreenPos(
                        ImVec2(labelPos.x + b * kPitch + (kSliderW - lw) * 0.5f,
                               labelPos.y));
                    ImGui::TextDisabled("%s", kBandLabel[b]);
                }
                ImGui::SetCursorPosX(colX);
                for (int b = 0; b < tng::Eq3::kBands; ++b) {
                    if (b > 0) ImGui::SameLine(0.0f, kGap);
                    char id[16];
                    std::snprintf(id, sizeof(id), "##eq%d%d", d, b);
                    float knob = mixer.eqKnob(d, b);
                    ImGui::VSliderFloat(id, ImVec2(kSliderW, kSliderH), &knob,
                                        0.0f, 1.0f, "",
                                        ImGuiSliderFlags_AlwaysClamp);
                    if (ImGui::IsItemActive()) mixer.setEqKnob(d, b, knob);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                        mixer.setEqKnob(d, b, 0.5f); // right-click = flat
                    if (ImGui::IsItemHovered()) {
                        const float gain = tng::Eq3::curve(knob);
                        // M5f: the tooltip only names the state - the
                        // right-click reset is documented, not shown here.
                        if (gain <= 0.001f) {
                            ImGui::SetTooltip("%s band: killed", kBandName[b]);
                        } else {
                            ImGui::SetTooltip(
                                "%s band: %+.1f dB", kBandName[b],
                                20.0 * std::log10(static_cast<double>(gain)));
                        }
                    }
                    // 0 dB center mark; overhangs the frame so it stays
                    // visible even when the handle sits exactly on it.
                    const ImVec2 s0 = ImGui::GetItemRectMin();
                    const ImVec2 s1 = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddLine(
                        ImVec2(s0.x - 4.0f, (s0.y + s1.y) * 0.5f),
                        ImVec2(s1.x + 4.0f, (s0.y + s1.y) * 0.5f),
                        IM_COL32(220, 220, 220, 220), 1.0f);
                }
                ImGui::SetCursorPosX(colX);
                for (int b = 0; b < tng::Eq3::kBands; ++b) {
                    if (b > 0) ImGui::SameLine(0.0f, kGap);
                    const bool killed = mixer.eqKnob(d, b) <= 0.001f;
                    if (killed) {
                        ImGui::PushStyleColor(
                            ImGuiCol_Button,
                            ImVec4(0.95f, 0.62f, 0.05f, 1.0f));
                        ImGui::PushStyleColor(
                            ImGuiCol_ButtonHovered,
                            ImVec4(1.00f, 0.72f, 0.15f, 1.0f));
                    }
                    char kid[16];
                    std::snprintf(kid, sizeof(kid), "##kill%d%d", d, b);
                    if (ImGui::Button(kid, ImVec2(kKill, kKill))) {
                        if (killed) {
                            mixer.setEqKnob(d, b, eqPreKill[d][b]); // restore
                        } else {
                            eqPreKill[d][b] = mixer.eqKnob(d, b);
                            mixer.setEqKnob(d, b, 0.0f);
                        }
                    }
                    if (killed) ImGui::PopStyleColor(2);
                    // M5g: no tooltip - the band slider's own tooltip
                    // already reports the killed/normal state.
                }
            }

            ImGui::Spacing();
            ImGui::TextDisabled("crossfader");
            float xf = mixer.crossfader();
            ImGui::SetNextItemWidth(kMixerWidth - 24.0f);
            ImGui::SliderFloat("##xf", &xf, -1.0f, 1.0f, "%.2f",
                               ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemActive()) mixer.setCrossfader(xf);
            // M5f: right-click centers the crossfader (like the EQ sliders
            // reset to flat); taught in the docs, no tooltip.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                xf = 0.0f;
                mixer.setCrossfader(0.0f);
            }

            // M5b: crossfader curve selector. M5c: persisted (written on
            // click, next boot restores it).
            static const char* kXfTips[3] = {
                "linear crossfade",
                "constant power (equal power, default)",
                "sharp cut (full until the last 5%)",
            };
            for (int c = 0; c < 3; ++c) {
                if (c > 0) ImGui::SameLine(0.0f, 4.0f);
                ImGui::PushID(c);
                if (ImGui::Selectable(kXfNames[c], mixer.xfCurve() == c, 0,
                                      ImVec2(38.0f, 0.0f))) {
                    mixer.setXfCurve(c);
                    settings.xfCurve = c;
                    saveSettings();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kXfTips[c]);
                ImGui::PopID();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("master");
            float master = mixer.masterGain() * 100.0f;
            ImGui::SetNextItemWidth(kMixerWidth - 24.0f);
            ImGui::SliderFloat("##master", &master, 0.0f, 100.0f, "%.0f%%",
                               ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemActive()) mixer.setMasterGain(master / 100.0f);
            if (ImGui::IsItemDeactivatedAfterEdit()) { // M5c: persist on release
                settings.masterGain = mixer.masterGain();
                saveSettings();
            }

            // M5b: the mixer's one-shot clip flag drives a short red light
            // (the soft clip itself lives in the engine, on the master).
            if (mixer.takeClipFlag()) uiClipFrames = 30;
            if (uiClipFrames > 0) --uiClipFrames;
            if (uiClipFrames > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(1.0f, 0.30f, 0.25f, 1.0f));
                ImGui::Text("CLIP");
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("clip");
            }
            ImGui::EndGroup();

            ImGui::SameLine();
            ImGui::BeginChild("deckB", ImVec2(panelW, availH), ImGuiChildFlags_None,
                              childFlags);
            if (const char* p = views[1].draw(decks[1], activeDeck == 1, false)) loadPath[1] = p;
            const ImVec2 b0 = ImGui::GetWindowPos();
            const ImVec2 b1(b0.x + ImGui::GetWindowSize().x, b0.y + ImGui::GetWindowSize().y);
            ImGui::EndChild();

            // Hovering a panel makes it the active deck (keyboard target).
            if (ImGui::IsMouseHoveringRect(a0, a1)) {
                activeDeck = 0;
            } else if (ImGui::IsMouseHoveringRect(b0, b1)) {
                activeDeck = 1;
            }
        } else {
            activeDeck = 0;
            if (const char* p = views[0].draw(decks[0], false, true)) loadPath[0] = p;
        }

        for (int i = 0; i < kDeckCount; ++i) {
            if (loadPath[i]) beginLoad(i, std::filesystem::u8path(loadPath[i]));
            // M4d: hot-cue save results bubble up to the status line.
            std::string noticeMsg;
            bool noticeErr = false;
            if (views[i].takeNotice(noticeMsg, noticeErr)) setStatus(noticeMsg, noticeErr);
            // M5c: the 10/16 button on either deck flips the shared range -
            // mirror it to the other view and persist (once per change).
            if (views[i].pitchRange() != settings.pitchRangePct) {
                settings.pitchRangePct = views[i].pitchRange();
                for (int j = 0; j < kDeckCount; ++j) {
                    views[j].setPitchRange(decks[j], settings.pitchRangePct);
                }
                saveSettings();
            }
        }

        bool anyLoading = false;
        for (int i = 0; i < kDeckCount; ++i) anyLoading = anyLoading || loading[i].has_value();
        if (anyLoading) ImGui::TextDisabled("loading...");

        ImGui::TextColored(statusError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                       : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                           "%s", status.c_str());
        if (ImGui::Button(mixMode ? "[Single]" : "[Mix]")) toggleMode();
        ImGui::SameLine();
        if (ImGui::Button("[settings]")) showSettings = !showSettings;
        ImGui::SameLine();
        ImGui::TextDisabled(
            "keys: Space play/pause (active deck) | 1-4 stems A, 7-0 stems B | "
            "+/- pitch (0 = reset) | M single/mix | Q / Esc quit | drop on a deck");
        ImGui::TextDisabled(
            "Shift+1-8 hot cue (active deck) | I / O loop in/out, L toggle, Shift+L exit, "
            "Alt+1-5 quick loop | nudge slider = hold for pitch bend (release snaps back)");
        ImGui::End();

        // --- M5c settings window (SPEC §4.3/§6): --------------------------
        // Immediate-apply + write-through save: every change hits the engine
        // or the views right away and lands in trekker-ng.json. Device and
        // buffer changes restart the audio device (brief dropout).
        if (showSettings) {
            ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f), ImGuiCond_Appearing);
            if (ImGui::Begin("settings", &showSettings)) {
                if (!deviceNamesLoaded) {
                    tng::AudioDevice::listPlaybackDevices(&deviceNames);
                    deviceNamesLoaded = true;
                }
                ImGui::TextDisabled("audio device");
                if (ImGui::BeginCombo(
                        "##dev", settings.deviceName.empty() ? "(default)"
                                                             : settings.deviceName.c_str())) {
                    for (const std::string& name : deviceNames) {
                        const bool sel = settings.deviceName == name;
                        if (ImGui::Selectable(name.empty() ? "(default)" : name.c_str(),
                                              sel)) {
                            if (!sel) {
                                settings.deviceName = name;
                                saveSettings();
                                applyDevice();
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("buffer size");
                static const char* kBufNames[] = {"64", "128", "256", "512", "1024"};
                static const int kBufFrames[] = {64, 128, 256, 512, 1024};
                int bufIdx = 2;
                for (int i = 0; i < 5; ++i) {
                    if (kBufFrames[i] == settings.bufferFrames) bufIdx = i;
                }
                if (ImGui::BeginCombo("##buf", kBufNames[bufIdx])) {
                    for (int i = 0; i < 5; ++i) {
                        const bool sel = i == bufIdx;
                        if (ImGui::Selectable(kBufNames[i], sel)) {
                            if (!sel) {
                                settings.bufferFrames = kBufFrames[i];
                                periodFrames = static_cast<uint32_t>(kBufFrames[i]);
                                saveSettings();
                                applyDevice();
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("pitch range");
                if (ImGui::BeginCombo("##pr",
                                      settings.pitchRangePct == 16.0f ? "16%" : "10%")) {
                    if (ImGui::Selectable("10%", settings.pitchRangePct == 10.0f)) {
                        settings.pitchRangePct = 10.0f;
                        for (int i = 0; i < kDeckCount; ++i) {
                            views[i].setPitchRange(decks[i], settings.pitchRangePct);
                        }
                        saveSettings();
                    }
                    if (ImGui::Selectable("16%", settings.pitchRangePct == 16.0f)) {
                        settings.pitchRangePct = 16.0f;
                        for (int i = 0; i < kDeckCount; ++i) {
                            views[i].setPitchRange(decks[i], settings.pitchRangePct);
                        }
                        saveSettings();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Checkbox("reversed pitch fader (up = slower)",
                                    &settings.pitchReversed)) {
                    for (int i = 0; i < kDeckCount; ++i) {
                        views[i].setPitchReversed(settings.pitchReversed);
                    }
                    saveSettings();
                }
                if (ImGui::Checkbox("start in Mix mode", &settings.bootMixMode)) {
                    saveSettings();
                }
                // M5e: size of the big VFD row (remaining time + pitch %);
                // applies live to both decks, saved on release.
                ImGui::TextDisabled("time + pitch readout size");
                float rs = settings.vfdTimePitchSize;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::SliderFloat("##rs", &rs, 12.0f, 32.0f, "%.0f px",
                                       ImGuiSliderFlags_AlwaysClamp)) {
                    settings.vfdTimePitchSize = rs;
                    for (int i = 0; i < kDeckCount; ++i) views[i].setReadoutSize(rs);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();
                ImGui::TextDisabled("crossfader curve");
                for (int c = 0; c < 3; ++c) {
                    if (c > 0) ImGui::SameLine(0.0f, 4.0f);
                    ImGui::PushID(100 + c);
                    if (ImGui::Selectable(kXfNames[c], mixer.xfCurve() == c, 0,
                                          ImVec2(38.0f, 0.0f))) {
                        mixer.setXfCurve(c);
                        settings.xfCurve = c;
                        saveSettings();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::End();
        }

        ImGui::Render();
        int dw = 0;
        int dh = 0;
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    for (auto& l : loading) {
        if (l) l->wait();
    }
    device.stop();
    device.shutdown();
    for (auto& d : decks) d.drainRetired();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(glctx);
    // M5f: persist the window state. Size only in windowed (non-maximized)
    // state so a fullscreen/maximized session never overwrites the
    // dimensions the next windowed boot restores.
    {
        const Uint32 fl = SDL_GetWindowFlags(window);
        settings.fullscreen = (fl & SDL_WINDOW_FULLSCREEN) != 0;
        if (!settings.fullscreen && !(fl & SDL_WINDOW_MAXIMIZED)) {
            int w = 0, h = 0;
            SDL_GetWindowSize(window, &w, &h);
            settings.windowWidth = w;
            settings.windowHeight = h;
        }
        saveSettings();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
