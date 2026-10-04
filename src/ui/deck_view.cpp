#include "deck_view.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tui {

namespace {

void formatTime(double seconds, char* out, size_t n) {
    if (seconds < 0.0) seconds = 0.0;
    const int m = static_cast<int>(seconds) / 60;
    const double s = seconds - static_cast<double>(m) * 60.0;
    std::snprintf(out, n, "%02d:%05.2f", m, s);
}

// 0xAARRGBB -> ImGui packed color. Muted lanes keep their hue, lose alpha,
// so the waveform stays visible but clearly inactive.
uint32_t stemColor(uint32_t color, bool on) {
    const uint32_t a = (color >> 24) & 0xFFu;
    const uint32_t r = (color >> 16) & 0xFFu;
    const uint32_t g = (color >> 8) & 0xFFu;
    const uint32_t b = color & 0xFFu;
    uint32_t alpha = (a == 0) ? 255 : a;
    if (!on) alpha = alpha * 40 / 100;
    return IM_COL32(r, g, b, alpha);
}

} // namespace

void WaveformCache::build(const tng::DeckData& data, int bins) {
    composite = Lane{};
    stems = {};
    if (bins <= 0) return;

    auto init = [&](Lane& lane, bool present) {
        lane.mins.assign(static_cast<size_t>(bins), 0.0f);
        lane.maxs.assign(static_cast<size_t>(bins), 0.0f);
        lane.peak = 0.0f;
        lane.present = present;
    };
    init(composite, data.frames > 0);
    for (int s = 0; s < tng::Deck::kStemCount; ++s) {
        const tng::Stem& st = data.stems[static_cast<size_t>(s)];
        init(stems[static_cast<size_t>(s)], data.frames > 0 && st.frames > 0);
    }

    if (data.frames <= 0) {
        composite.peak = 1.0f;
        for (Lane& lane : stems) lane.peak = 1.0f;
        return;
    }

    // Single pass over all frames: the composite and the 4 stem lanes are
    // filled together (one-time, load side, UI thread - SPEC §4.7).
    for (int64_t i = 0; i < data.frames; ++i) {
        const int b = static_cast<int>(i * bins / data.frames);
        float mix = 0.0f;
        for (int s = 0; s < tng::Deck::kStemCount; ++s) {
            const tng::Stem& st = data.stems[static_cast<size_t>(s)];
            float v = 0.0f;
            if (st.frames > i) {
                v = 0.5f * (st.data[static_cast<size_t>(i) * 2] +
                            st.data[static_cast<size_t>(i) * 2 + 1]);
            }
            Lane& lane = stems[static_cast<size_t>(s)];
            if (lane.present) {
                if (v < lane.mins[static_cast<size_t>(b)]) lane.mins[static_cast<size_t>(b)] = v;
                if (v > lane.maxs[static_cast<size_t>(b)]) lane.maxs[static_cast<size_t>(b)] = v;
                const float av = std::fabs(v);
                if (av > lane.peak) lane.peak = av;
            }
            mix += v;
        }
        if (composite.present) {
            Lane& lane = composite;
            if (mix < lane.mins[static_cast<size_t>(b)]) lane.mins[static_cast<size_t>(b)] = mix;
            if (mix > lane.maxs[static_cast<size_t>(b)]) lane.maxs[static_cast<size_t>(b)] = mix;
            const float av = std::fabs(mix);
            if (av > lane.peak) lane.peak = av;
        }
    }

    auto normalize = [](Lane& lane) {
        if (lane.peak < 1e-6f) lane.peak = 1.0f;
        for (float& v : lane.mins) v /= lane.peak;
        for (float& v : lane.maxs) v /= lane.peak;
    };
    normalize(composite);
    for (Lane& lane : stems) normalize(lane);
}

void DeckView::drawLane(tng::Deck& deck, const tng::DeckData& data,
                        const WaveformCache::Lane& lane, uint32_t color, float height,
                        const char* label) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    if (w < 32.0f) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + height), IM_COL32(14, 14, 18, 255));

    const int bins = static_cast<int>(lane.mins.size());
    if (lane.present && bins > 0 && data.frames > 0) {
        const float mid = pos.y + height * 0.5f;
        const float half = height * 0.42f;
        const int pxCount = static_cast<int>(w);
        for (int x = 0; x < pxCount; ++x) {
            int b = static_cast<int>(static_cast<int64_t>(x) * bins / pxCount);
            if (b >= bins) b = bins - 1;
            const float y0 = mid - lane.maxs[static_cast<size_t>(b)] * half;
            const float y1 = mid - lane.mins[static_cast<size_t>(b)] * half;
            const float sx = pos.x + static_cast<float>(x);
            dl->AddLine(ImVec2(sx, y0), ImVec2(sx, std::max(y1, y0 + 1.0f)), color);
        }
    } else {
        const float mid = pos.y + height * 0.5f;
        dl->AddLine(ImVec2(pos.x, mid), ImVec2(pos.x + w, mid), IM_COL32(70, 70, 80, 255));
    }

    // Playhead crosses every lane.
    if (data.frames > 0) {
        const double posFrames = deck.positionFrames();
        const float t = static_cast<float>(std::clamp(
            posFrames / std::max(1.0, static_cast<double>(data.frames)), 0.0, 1.0));
        const float px = pos.x + t * w;
        dl->AddLine(ImVec2(px, pos.y), ImVec2(px, pos.y + height), IM_COL32(255, 80, 80, 255),
                    2.0f);
    }

    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + height), IM_COL32(60, 60, 70, 255));
    if (label != nullptr) {
        dl->AddText(ImVec2(pos.x + 8.0f, pos.y + 3.0f), color, label);
    }

    ImGui::Dummy(ImVec2(w, height));
    if (data.frames > 0 && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - pos.x) / w, 0.0f, 1.0f);
        deck.requestSeek(static_cast<int64_t>(t * static_cast<double>(data.frames)));
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

const char* DeckView::draw(tng::Deck& deck) {
    const tng::DeckData* data = deck.track();
    const char* loadPath = nullptr;

    if (data && wfDirty_) {
        wf_.build(*data);
        wfDirty_ = false;
    }

    // --- title: artist - title ---------------------------------------------------
    if (data) {
        if (data->artist.empty()) {
            ImGui::Text("%s", data->title.c_str());
        } else {
            ImGui::Text("%s - %s", data->artist.c_str(), data->title.c_str());
        }
    } else {
        ImGui::TextDisabled("no track loaded");
    }

    // --- waveforms: composite on top, 4 stem lanes below (all full width) ------
    if (data) {
        drawLane(deck, *data, wf_.composite, IM_COL32(90, 200, 130, 255), 150.0f, nullptr);
        for (int i = 0; i < tng::Deck::kStemCount; ++i) {
            ImGui::Spacing();
            const tng::Stem& st = data->stems[static_cast<size_t>(i)];
            char laneLabel[48];
            if (st.frames > 0) {
                std::snprintf(laneLabel, sizeof(laneLabel), "%d %s", i + 1,
                              st.name.empty() ? "stem" : st.name.c_str());
            } else {
                std::snprintf(laneLabel, sizeof(laneLabel), "%d (empty)", i + 1);
            }
            drawLane(deck, *data, wf_.stems[static_cast<size_t>(i)],
                     stemColor(st.color, deck.stemOn(i)), 38.0f, laneLabel);
        }
    } else {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = 150.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(14, 14, 18, 255));
        dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(60, 60, 70, 255));
        dl->AddText(ImVec2(pos.x + 16.0f, pos.y + h * 0.5f - 8.0f),
                    IM_COL32(150, 150, 160, 255),
                    "drop a track folder or .zip anywhere in this window");
        ImGui::Dummy(ImVec2(w, h)); // keeps the cursor below the box
    }

    // --- transport + time -------------------------------------------------------
    if (data) {
        ImGui::Spacing();
        const bool atEnd =
            deck.positionFrames() >= static_cast<double>(data->frames) - 1.0;
        const char* playLabel = deck.playing() ? "Pause" : "Play";
        if (ImGui::Button(playLabel, ImVec2(80.0f, 0.0f))) {
            if (!deck.playing() && atEnd) deck.requestSeek(0); // restart from end
            deck.setPlaying(!deck.playing());
        }
        ImGui::SameLine();
        char t1[32], t2[32];
        const double fs = static_cast<double>(std::max(data->sampleRate, 1u));
        formatTime(deck.positionFrames() / fs, t1, sizeof(t1));
        formatTime(static_cast<double>(data->frames) / fs, t2, sizeof(t2));
        ImGui::Text("%s / %s", t1, t2);
        if (data->bpm > 0.0f) {
            ImGui::SameLine();
            const double beats = deck.positionFrames() / fs *
                                 static_cast<double>(data->bpm) * deck.rate() / 60.0;
            ImGui::TextDisabled("beat %.1f | %.1f BPM", std::fmod(beats, 4.0) + 1.0,
                                static_cast<double>(data->bpm) * deck.rate());
        }

        // --- vertical pitch fader (SPEC §6), reversed like DJ gear: -------
        // top = slow, bottom = fast. The widget edits the inverted value, so
        // dragging down raises the pitch; the readout shows the real pitch.
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::BeginGroup();
        ImGui::TextDisabled("pitch");
        float shown = -pitchPct_;
        ImGui::VSliderFloat("##pitch", ImVec2(34.0f, 90.0f), &shown, -10.0f, 10.0f, "",
                            ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActive()) {
            pitchPct_ = -shown;
            deck.setRate(1.0 + static_cast<double>(pitchPct_) / 100.0);
        } else {
            pitchPct_ = static_cast<float>((deck.rate() - 1.0) * 100.0); // idle: mirror
        }
        ImGui::Text("%+.2f%%", pitchPct_);
        ImGui::EndGroup();
    }

    // --- stem toggles (colored, SPEC §6) ---------------------------------------
    if (data) {
        ImGui::Spacing();
        for (int i = 0; i < tng::Deck::kStemCount; ++i) {
            const tng::Stem& st = data->stems[static_cast<size_t>(i)];
            const bool present = st.frames > 0;
            char label[32];
            std::snprintf(label, sizeof(label), "%d %s", i + 1,
                          present && !st.name.empty() ? st.name.c_str() : "empty");
            if (!present) ImGui::BeginDisabled();
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(
                static_cast<float>((st.color >> 16) & 0xFF) / 255.0f,
                static_cast<float>((st.color >> 8) & 0xFF) / 255.0f,
                static_cast<float>(st.color & 0xFF) / 255.0f, 1.0f));
            bool on = deck.stemOn(i);
            if (ImGui::Checkbox(label, &on)) deck.setStem(i, on);
            ImGui::PopStyleColor();
            ImGui::PopID();
            if (!present) ImGui::EndDisabled();
            if (i != tng::Deck::kStemCount - 1) ImGui::SameLine();
        }
    }

    // --- load box --------------------------------------------------------------
    ImGui::Spacing();
    const float loadW = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(loadW - 90.0f);
    ImGui::InputText("##path", pathBuf_, sizeof(pathBuf_));
    ImGui::SameLine();
    if (ImGui::Button("Load") && pathBuf_[0] != '\0') {
        loadPath = pathBuf_;
    }

    return loadPath;
}

} // namespace tui
