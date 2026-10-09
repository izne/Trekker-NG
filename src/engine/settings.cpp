#include "settings.h"

#include "json.hpp"

#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace tng {

namespace {

// Clamp into the ranges the UI actually offers (SPEC §4.3 / M5c list);
// values come from a user-editable file and must never reach the engine
// unchecked.
void sanitize(Settings& s) {
    if (s.bufferFrames < 64) s.bufferFrames = 64;
    if (s.bufferFrames > 1024) s.bufferFrames = 1024;
    if (s.pitchRangePct < 12.0f) {
        s.pitchRangePct = 10.0f;
    } else {
        s.pitchRangePct = 16.0f;
    }
    if (s.xfCurve < 0) s.xfCurve = 0;
    if (s.xfCurve > 2) s.xfCurve = 2;
    if (s.masterGain < 0.0f) s.masterGain = 0.0f;
    if (s.masterGain > 1.0f) s.masterGain = 1.0f;
    if (s.vfdTimePitchSize < 12.0f) s.vfdTimePitchSize = 12.0f;
    if (s.vfdTimePitchSize > 32.0f) s.vfdTimePitchSize = 32.0f;
    if (s.windowWidth < 800) s.windowWidth = 800;
    if (s.windowWidth > 7680) s.windowWidth = 7680;
    if (s.windowHeight < 480) s.windowHeight = 480;
    if (s.windowHeight > 4320) s.windowHeight = 4320;
}

} // namespace

std::string Settings::toJson() const {
    Settings s = *this;
    sanitize(s);
    json j;
    j["deviceName"] = s.deviceName;
    j["bufferFrames"] = s.bufferFrames;
    j["pitchRangePct"] = s.pitchRangePct;
    j["pitchReversed"] = s.pitchReversed;
    j["bootMixMode"] = s.bootMixMode;
    j["xfCurve"] = s.xfCurve;
    j["masterGain"] = s.masterGain;
    j["vfdTimePitchSize"] = s.vfdTimePitchSize;
    j["windowWidth"] = s.windowWidth;
    j["windowHeight"] = s.windowHeight;
    j["fullscreen"] = s.fullscreen;
    return j.dump(2) + "\n";
}

bool Settings::fromJson(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (...) {
        return false;
    }
    if (!j.is_object()) return false;
    deviceName = j.value("deviceName", deviceName);
    bufferFrames = j.value("bufferFrames", bufferFrames);
    pitchRangePct = j.value("pitchRangePct", pitchRangePct);
    pitchReversed = j.value("pitchReversed", pitchReversed);
    bootMixMode = j.value("bootMixMode", bootMixMode);
    xfCurve = j.value("xfCurve", xfCurve);
    masterGain = j.value("masterGain", masterGain);
    vfdTimePitchSize = j.value("vfdTimePitchSize", vfdTimePitchSize);
    windowWidth = j.value("windowWidth", windowWidth);
    windowHeight = j.value("windowHeight", windowHeight);
    fullscreen = j.value("fullscreen", fullscreen);
    sanitize(*this);
    return true;
}

bool Settings::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary); // path overload = wide-safe on Win
    if (!in) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    return fromJson(buf.str());
}

bool Settings::save(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << toJson();
    return static_cast<bool>(out);
}

} // namespace tng
