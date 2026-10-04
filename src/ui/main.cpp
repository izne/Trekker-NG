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
#include "track_loader.h"

#include "SDL.h"
#include "SDL_opengl.h"
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

#include "font_dseg.inc" // embedded DSEG7 Classic (SIL OFL 1.1), M4display

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr uint32_t kPeriodFrames = 256; // SPEC §4.7
constexpr int kDeckCount = 2;
constexpr float kMixerWidth = 150.0f; // center column in Mix mode
constexpr float kStatusReserve = 64.0f; // bottom lines under the deck children

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

    SDL_Window* window = SDL_CreateWindow(
        "Trekker-NG", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1100, 660,
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

    std::array<tng::Deck, kDeckCount> decks;
    std::array<tui::DeckView, kDeckCount> views;
    tng::Mixer mixer;
    tng::AudioDevice device;
    bool deviceUp = false;
    uint32_t devRate = 0;

    // Always boots Single (user decision 2026-10-04): the M toggle is
    // session-only until the M5 settings screen adds real persistence.
    bool mixMode = false;
    int activeDeck = 0;   // deck the global keys target (hovered panel in Mix)

    // Single mode: deck A through the crossfader at exactly gain 1.0
    // (crossfader -1) -> loudness identical to M3; Mix: centered, both
    // decks audible. Deck B is registered with the device in both modes (a
    // paused deck renders near-free silence) so toggling never re-inits.
    mixer.setCrossfader(-1.0f);

    std::string status = "drop a track folder or .zip, or paste a path";
    bool statusError = false;
    std::array<std::optional<std::future<tng::LoadResult>>, kDeckCount> loading;

    auto setStatus = [&](const std::string& s, bool err) {
        status = s;
        statusError = err;
        std::fprintf(stderr, "[%s] %s\n", err ? "error" : "info", s.c_str());
        std::fflush(stderr);
    };

    // Pitch from the keyboard, same feel as the console: 0.10% steps,
    // Shift = 0.01% fine, snapped to the 0.01% grid, clamped to +/-10%.
    // Acts on the active deck (hover a panel to switch in Mix mode).
    auto pitchBy = [&](int dir) {
        tng::Deck& d = decks[activeDeck];
        const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        const double step = shift ? 0.01 : 0.10;
        double pct = std::round(((d.rate() - 1.0) * 100.0 +
                                 static_cast<double>(dir) * step) *
                                100.0) /
                     100.0;
        pct = std::max(-10.0, std::min(10.0, pct));
        d.setRate(1.0 + pct / 100.0);
    };

    auto startDevice = [&](uint32_t rate) -> bool {
        std::string err;
        if (!device.init(&decks[0], &decks[1], &mixer, rate, kPeriodFrames, &err)) {
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

    auto beginLoad = [&](int di, const std::filesystem::path& path) {
        if (loading[di]) {
            setStatus(std::string("deck ") + deckName(di) +
                          ": still loading - wait for the current track",
                      true);
            return;
        }
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
                    // SPEC §6.155: 1-4 stems on deck A, 7-0 on deck B.
                    // Shift+digit is a different character on some layouts
                    // (German "!§$"): only the plain digits toggle stems.
                    case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4:
                        if (!(e.key.keysym.mod & KMOD_SHIFT)) {
                            decks[0].toggleStem(e.key.keysym.sym - SDLK_1);
                        }
                        break;
                    case SDLK_7: case SDLK_8: case SDLK_9:
                        if (!(e.key.keysym.mod & KMOD_SHIFT)) {
                            decks[1].toggleStem(e.key.keysym.sym - SDLK_7);
                        }
                        break;
                    case SDLK_0:
                        // '0' is the pitch-reset key in Single mode (M3) and
                        // deck B's stem 4 in Mix mode (SPEC §6.155) - the
                        // TEXTINPUT handler below skips it while in Mix so one
                        // keypress never does both.
                        if (mixMode && !(e.key.keysym.mod & KMOD_SHIFT)) {
                            decks[1].toggleStem(3);
                        }
                        break;
                    case SDLK_m:
                        toggleMode();
                        break;
                    case SDLK_q: case SDLK_ESCAPE:
                        running = false;
                        break;
                    default: break;
                }
            } else if (e.type == SDL_TEXTINPUT && !io.WantTextInput) {
                // Printable keys via their produced character, so every layout
                // works (US "=+-_[]0", German "+ -" keys, German Shift+0 "=").
                for (const char* c = e.text.text; *c != '\0'; ++c) {
                    switch (*c) {
                        case '+': case '=': case ']': pitchBy(+1); break;
                        case '-': case '_': case '[': pitchBy(-1); break;
                        case '0':
                            if (!mixMode) decks[activeDeck].setRate(1.0);
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
            if (const char* p = views[0].draw(decks[0], activeDeck == 0)) loadPath[0] = p;
            const ImVec2 a0 = ImGui::GetWindowPos();
            const ImVec2 a1(a0.x + ImGui::GetWindowSize().x, a0.y + ImGui::GetWindowSize().y);
            ImGui::EndChild();

            ImGui::SameLine();
            // --- mixer column: line faders, crossfader, master (SPEC §4.6) ---
            ImGui::BeginGroup();
            ImGui::TextDisabled("mixer");
            ImGui::BeginGroup();
            ImGui::TextDisabled("A");
            float lineA = mixer.lineGain(0);
            ImGui::VSliderFloat("##lineA", ImVec2(38.0f, 100.0f), &lineA, 0.0f, 1.0f,
                                "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemActive()) mixer.setLineGain(0, lineA);
            ImGui::EndGroup();
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextDisabled("B");
            float lineB = mixer.lineGain(1);
            ImGui::VSliderFloat("##lineB", ImVec2(38.0f, 100.0f), &lineB, 0.0f, 1.0f,
                                "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemActive()) mixer.setLineGain(1, lineB);
            ImGui::EndGroup();

            ImGui::Spacing();
            ImGui::TextDisabled("crossfader");
            float xf = mixer.crossfader();
            ImGui::SetNextItemWidth(kMixerWidth - 24.0f);
            ImGui::SliderFloat("##xf", &xf, -1.0f, 1.0f, "%.2f",
                               ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemActive()) mixer.setCrossfader(xf);

            ImGui::Spacing();
            ImGui::TextDisabled("master");
            float master = mixer.masterGain() * 100.0f;
            ImGui::SetNextItemWidth(kMixerWidth - 24.0f);
            ImGui::SliderFloat("##master", &master, 0.0f, 100.0f, "%.0f%%",
                               ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemActive()) mixer.setMasterGain(master / 100.0f);
            ImGui::EndGroup();

            ImGui::SameLine();
            ImGui::BeginChild("deckB", ImVec2(panelW, availH), ImGuiChildFlags_None,
                              childFlags);
            if (const char* p = views[1].draw(decks[1], activeDeck == 1)) loadPath[1] = p;
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
            if (const char* p = views[0].draw(decks[0], false)) loadPath[0] = p;
        }

        for (int i = 0; i < kDeckCount; ++i) {
            if (loadPath[i]) beginLoad(i, std::filesystem::u8path(loadPath[i]));
        }

        bool anyLoading = false;
        for (int i = 0; i < kDeckCount; ++i) anyLoading = anyLoading || loading[i].has_value();
        if (anyLoading) ImGui::TextDisabled("loading...");

        ImGui::TextColored(statusError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                       : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                           "%s", status.c_str());
        if (ImGui::Button(mixMode ? "[Single]" : "[Mix]")) toggleMode();
        ImGui::SameLine();
        ImGui::TextDisabled(
            "keys: Space play/pause (active deck) | 1-4 stems A, 7-0 stems B | "
            "+/- pitch (0 = reset) | M single/mix | Q / Esc quit | drop on a deck");
        ImGui::End();

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
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
