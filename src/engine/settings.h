#pragma once

#include <filesystem>
#include <string>

namespace tng {

// M5c user settings (SPEC §6): persisted next to the exe as
// trekker-ng.json (same nlohmann JSON stack as meta.json / cue sidecars).
// Pure data + string/JSON conversion - no UI or audio dependencies, safe
// to unit-test in-memory; the path-based load()/save() are thin IO
// wrappers like track_loader's saveCues (exercised by the app, not the
// suite). UI thread only - the audio callback never sees any of this.
struct Settings {
    std::string deviceName;      // "" = system default playback device
    int bufferFrames = 256;      // callback size, clamped to 64..1024
    float pitchRangePct = 10.0f; // 10 or 16 (SPEC §4.3), seeds both views
    bool pitchReversed = true;   // DJ-style default: up = slower (§4.3)
    bool bootMixMode = false;    // Single at boot unless Mix was saved
    int xfCurve = 1;             // Mixer::kXfConstantPower
    float masterGain = 0.7f;     // matches the Mixer default

    // JSON text (round-trip core). fromJson keeps the defaults for any
    // missing/invalid field and returns false only when the text is not
    // parseable JSON at all (corrupt file = ignore, like the cue sidecars).
    std::string toJson() const;
    bool fromJson(const std::string& text);

    // Path IO: load() returns false (keeping defaults) when the file is
    // missing or unreadable; save() returns false on write failure.
    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;
};

} // namespace tng
