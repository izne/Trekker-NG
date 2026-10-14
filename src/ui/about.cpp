#include "about.h"

#include "SDL.h"          // SDL_GetVersion: the actual runtime SDL2
#include "imgui.h"
#include "json.hpp"       // NLOHMANN_JSON_VERSION_*
#include "miniaudio.h"    // MA_VERSION_STRING
#include <soundtouch/SoundTouch.h> // SOUNDTOUCH_VERSION (pacman)

#include <cstdio>

#ifndef TREKKER_VERSION
#define TREKKER_VERSION "dev" // IDE builds without the CMake define
#endif

namespace tui {
namespace {

constexpr const char* kAboutPopup = "about";
constexpr const char* kRepoUrl = "https://github.com/izne/Trekker-NG";

// miniz's and doctest's vendored copies are frozen (miniz predates version
// macros, doctest's header links against its implementation TU): keep these
// in sync with the "miniz.c 3.1.2" banner in third_party/miniz/miniz.h and
// DOCTEST_VERSION_STR in third_party/doctest.h.
constexpr const char* kMinizVersion = "3.1.2";
constexpr const char* kDoctestVersion = "2.4.12";

void libRow(const char* name, const char* version, const char* role,
            const char* license) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (version != nullptr && version[0] != '\0') {
        ImGui::Text("%s %s", name, version);
    } else {
        ImGui::TextUnformatted(name);
    }
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(role);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(license);
}

} // namespace

void drawAbout(bool* pOpen) {
    if (pOpen == nullptr) return;
    // First frame after the [about] click: put the popup on the stack.
    // While it is open, BeginPopupModal below keeps pOpen in sync (it
    // resets the flag when the popup is not open and closes it when the
    // flag goes false - X button, Esc, whatever).
    if (*pOpen && !ImGui::IsPopupOpen(kAboutPopup)) {
        ImGui::OpenPopup(kAboutPopup);
    }
    // M11: always centered (ImGui's default is FirstUseEver, which would
    // let a dragged position in imgui.ini stick), and not draggable.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kAboutPopup, pOpen,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoMove)) {
        return;
    }

    char jsonVer[24];
    std::snprintf(jsonVer, sizeof(jsonVer), "%d.%d.%d",
                  NLOHMANN_JSON_VERSION_MAJOR, NLOHMANN_JSON_VERSION_MINOR,
                  NLOHMANN_JSON_VERSION_PATCH);
    SDL_version sdl{};
    SDL_GetVersion(&sdl);
    char sdlVer[24];
    std::snprintf(sdlVer, sizeof(sdlVer), "%d.%d.%d", sdl.major, sdl.minor,
                  sdl.patch);
    char title[64];
    std::snprintf(title, sizeof(title), "Trekker-NG %s", TREKKER_VERSION);

    ImGui::TextUnformatted(title);
    ImGui::TextDisabled("a minimalistic stem-based DJ playing software");
    ImGui::Spacing();
    ImGui::TextDisabled("uses these third-party libraries:");

    if (ImGui::BeginTable("##libs", 3,
                          ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("library");
        ImGui::TableSetupColumn("role");
        ImGui::TableSetupColumn("license");
        ImGui::TableHeadersRow();
        libRow("miniaudio", MA_VERSION_STRING, "audio device, decode, WAV encode",
               "public domain / MIT-0");
        libRow("Dear ImGui", IMGUI_VERSION, "immediate-mode UI", "MIT");
        libRow("SDL2", sdlVer, "window and OpenGL context", "zlib");
        libRow("miniz", kMinizVersion, "zip track reading", "public domain");
        libRow("nlohmann/json", jsonVer, "meta.json and settings", "MIT");
        libRow("SoundTouch", SOUNDTOUCH_VERSION, "BPM auto-detection",
               "LGPL 2.1");
        libRow("libmp3lame", nullptr, "MP3 recording (optional DLL)",
               "LGPL 2.1");
        libRow("DSEG7 Classic", nullptr, "VFD readout font (embedded)",
               "SIL OFL 1.1");
        libRow("doctest", kDoctestVersion, "test suite (development)", "MIT");
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("source:");
    ImGui::SameLine();
    // M11: a real hyperlink - hover underlines it, click opens the default
    // browser (the SDL2 backend wires Platform_OpenInShellFn to
    // SDL_OpenURL), right-click offers "Copy Link" to the clipboard.
    ImGui::TextLinkOpenURL(kRepoUrl);
    ImGui::TextDisabled("LGPL/OFL license texts ship in docs/.");
    ImGui::Spacing();
    if (ImGui::Button("close")) {
        *pOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace tui
