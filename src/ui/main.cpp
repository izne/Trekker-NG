// Trekker-NG M3 UI: SDL2 window + OpenGL 3 + Dear ImGui, one deck (SPEC §6/§8).
//
// Track loading runs on a worker thread (SPEC §4.7: prepared off-thread),
// then publishTrack() swaps it in atomically; the audio callback adopts it on
// the next block. Dropping a folder or .zip anywhere on the window loads it.

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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <optional>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr uint32_t kPeriodFrames = 256; // SPEC §4.7

void showError(const char* what) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Trekker-NG", what, nullptr);
}

} // namespace

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
    // No ImGui keyboard nav: global shortcuts (Space, 1-4) must never fire
    // twice - once through nav activation, once through SDL_KEYDOWN.
    ImGui_ImplSDL2_InitForOpenGL(window, glctx);
    ImGui_ImplOpenGL3_Init("#version 130");

    tng::Deck deck;
    tng::Mixer mixer;
    tng::AudioDevice device;
    bool deviceUp = false;
    uint32_t devRate = 0;

    std::string status = "drop a track folder or .zip, or paste a path";
    bool statusError = false;
    std::optional<std::future<tng::LoadResult>> loading;
    tui::DeckView view;

    auto setStatus = [&](const std::string& s, bool err) {
        status = s;
        statusError = err;
        std::fprintf(stderr, "[%s] %s\n", err ? "error" : "info", s.c_str());
        std::fflush(stderr);
    };

    // Pitch from the keyboard, same feel as the console: 0.10% steps,
    // Shift = 0.01% fine, snapped to the 0.01% grid, clamped to +/-10%.
    auto pitchBy = [&](int dir) {
        const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        const double step = shift ? 0.01 : 0.10;
        double pct = std::round(((deck.rate() - 1.0) * 100.0 +
                                 static_cast<double>(dir) * step) *
                                100.0) /
                     100.0;
        pct = std::max(-10.0, std::min(10.0, pct));
        deck.setRate(1.0 + pct / 100.0);
    };

    auto startDevice = [&](uint32_t rate) -> bool {
        std::string err;
        if (!device.init(&deck, nullptr, &mixer, rate, kPeriodFrames, &err)) {
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

    auto beginLoad = [&](const std::filesystem::path& path) {
        if (loading) {
            setStatus("still loading - wait for the current track", true);
            return;
        }
        loading = std::async(std::launch::async, [path] { return tng::loadTrack(path); });
        setStatus("loading...", false);
    };

    auto onLoaded = [&](tng::LoadResult res) {
        if (!res.ok()) {
            setStatus("error: " + res.error, true);
            return;
        }
        const uint32_t rate = res.data->sampleRate;
        if (deviceUp && rate != devRate) {
            // Sample rate changed: restart the device at the track's rate.
            device.stop();
            device.shutdown();
            deviceUp = false;
        }
        deck.publishTrack(std::move(res.data)); // hot swap (SPEC §4.7)
        view.onTrackChanged();
        if (!deviceUp) startDevice(rate);

        const tng::DeckData* d = deck.track();
        char buf[320];
        std::snprintf(buf, sizeof(buf), "loaded: %s%s%s | %u Hz | %.1f s",
                      d->title.c_str(), d->artist.empty() ? "" : " - ",
                      d->artist.c_str(), d->sampleRate,
                      static_cast<double>(d->frames) / static_cast<double>(d->sampleRate));
        setStatus(buf, !deviceUp);
    };

    if (argc > 1) beginLoad(std::filesystem::path(argv[1]));

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
                    beginLoad(std::filesystem::u8path(e.drop.file));
                    SDL_free(e.drop.file);
                }
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat && !io.WantTextInput) {
                switch (e.key.keysym.sym) {
                    case SDLK_SPACE: {
                        const tng::DeckData* d = deck.track();
                        if (!deck.playing() && d &&
                            deck.positionFrames() >= static_cast<double>(d->frames) - 1.0) {
                            deck.requestSeek(0); // restart from the end
                        }
                        deck.setPlaying(!deck.playing());
                        break;
                    }
                    case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4:
                        // Shift+digit is a different character on some layouts
                        // (German "!§$"): only the plain digits toggle stems.
                        if (!(e.key.keysym.mod & KMOD_SHIFT)) {
                            deck.toggleStem(e.key.keysym.sym - SDLK_1);
                        }
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
                        case '0': deck.setRate(1.0); break;
                        default: break;
                    }
                }
            }
        }

        if (loading &&
            loading->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            onLoaded(loading->get());
            loading.reset();
        }
        deck.drainRetired(); // release swapped-out data once adopted

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoScrollbar);
        if (const char* path = view.draw(deck)) {
            beginLoad(std::filesystem::u8path(path));
        }
        if (loading) {
            ImGui::TextDisabled("loading...");
        }
        ImGui::TextColored(statusError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f)
                                       : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
                           "%s", status.c_str());
        ImGui::TextDisabled(
            "keys: Space play/pause | 1-4 stem toggle | +/- pitch (Shift = fine, 0 = "
            "reset) | Q / Esc quit | click the waveform to seek");
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

    if (loading) loading->wait();
    device.stop();
    device.shutdown();
    deck.drainRetired();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(glctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
