#include "deck_view.h"

#include "imgui.h"
#include "track_loader.h" // saveCues after hot-cue edits (M4d)

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // also pulls NOMINMAX (std::min/max stay intact)
#endif
#include <windows.h>
#include <commdlg.h> // M5e: GetOpenFileNameW zip picker for the Load button
#endif

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

// --- VFD readout (M4display) ------------------------------------------------
// One dark inset box with mixed-font segments: DSEG7 cyan numbers (the
// default font is merged into vfdFont, so '+', '%', '/' fall back to it
// automatically) next to plain-font words like "beat"/"BPM".
// M5e: `size` overrides the DSEG segment size (0 = the standard 15 px);
// ImGui 1.92 rasterizes glyphs on demand, so any size renders crisp from
// the single loaded font. Word segments keep the regular font size.
struct VfdSeg {
    const char* text;
    bool dseg;
};

void vfdLine(const VfdSeg* segs, int count, float size = 0.0f) {
    static const ImU32 kInset = IM_COL32(8, 12, 14, 255);
    static const ImU32 kCyan = IM_COL32(96, 244, 224, 255);
    static const ImU32 kWord = IM_COL32(170, 178, 186, 255);
    const float dsegSize = size > 0.0f ? size : kVfdFontSize;
    const float padX = 4.0f, padY = 2.0f;
    if (count > 8) count = 8;
    float widths[8], heights[8];
    float total = 0.0f, maxH = 0.0f;
    for (int i = 0; i < count; ++i) {
        ImVec2 ts;
        if (segs[i].dseg && vfdFont) {
            ImGui::PushFont(vfdFont, dsegSize);
            ts = ImGui::CalcTextSize(segs[i].text);
            ImGui::PopFont();
        } else {
            ts = ImGui::CalcTextSize(segs[i].text);
        }
        widths[i] = ts.x;
        heights[i] = ts.y;
        total += ts.x;
        maxH = std::max(maxH, ts.y);
    }
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 box(total + 2.0f * padX, maxH + 2.0f * padY);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + box.x, pos.y + box.y), kInset);
    float x = pos.x + padX;
    for (int i = 0; i < count; ++i) {
        const float y = pos.y + padY + (maxH - heights[i]) * 0.5f;
        if (segs[i].dseg && vfdFont) {
            dl->AddText(vfdFont, dsegSize, ImVec2(x, y), kCyan, segs[i].text);
        } else {
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y),
                        kWord, segs[i].text);
        }
        x += widths[i];
    }
    ImGui::Dummy(box);
}

// M5e: native zip picker for the Load... button. Returns an empty string
// on cancel/failure. `hwnd` is the owner window (HWND, may be null).
std::string pickZipDialog(void* hwnd) {
#ifdef _WIN32
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(hwnd);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Track zip (*.zip)\0*.zip\0All files (*.*)\0*.*\0";
    ofn.lpstrTitle = L"Load a Trekker-NG track zip";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    return std::filesystem::path(file).u8string();
#else
    (void)hwnd;
    return {};
#endif
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
        // White, not the stem color: the label sits over the waveform and
        // disappears into same-colored peaks (worst when the lane is muted
        // and its color is dimmed). M5e: a dark +1/+1 shadow copy first so
        // it also survives bright peaks under the white text.
        dl->AddText(ImVec2(pos.x + 9.0f, pos.y + 4.0f), IM_COL32(0, 0, 0, 140),
                    label);
        dl->AddText(ImVec2(pos.x + 8.0f, pos.y + 3.0f), IM_COL32(255, 255, 255, 255),
                    label);
    }

    ImGui::Dummy(ImVec2(w, height));
    if (data.frames > 0 && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - pos.x) / w, 0.0f, 1.0f);
        deck.requestSeek(static_cast<int64_t>(t * static_cast<double>(data.frames)));
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

void drawVuBar(ImDrawList* dl, float x, float y, float w, float h, float level) {
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(14, 14, 18, 255));
    const float fill = level * (h - 2.0f);
    const ImU32 color = level > 0.9f   ? IM_COL32(255, 70, 60, 255)
                        : level > 0.7f ? IM_COL32(255, 200, 60, 255)
                                       : IM_COL32(90, 210, 120, 255);
    dl->AddRectFilled(ImVec2(x + 1.0f, y + h - 1.0f - fill),
                      ImVec2(x + w - 1.0f, y + h - 1.0f), color);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(60, 60, 70, 255));
}

const char* DeckView::draw(tng::Deck& deck, bool active, bool showVU) {
    const tng::DeckData* data = deck.track();
    const char* loadPath = nullptr;

    if (data && wfDirty_) {
        wf_.build(*data);
        wfDirty_ = false;
    }

    // --- title: artist - title ---------------------------------------------------
    // The active deck's title is accented so it is obvious which deck the
    // global keys (Space, pitch) target.
    if (active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.65f, 1.0f, 1.0f));
    if (data) {
        if (data->artist.empty()) {
            ImGui::Text("%s", data->title.c_str());
        } else {
            ImGui::Text("%s - %s", data->artist.c_str(), data->title.c_str());
        }
    } else if (active) {
        ImGui::Text("no track loaded");
    } else {
        ImGui::TextDisabled("no track loaded");
    }
    if (active) ImGui::PopStyleColor();

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
        // Left column (M5e): everything that is not a slider - buttons,
        // the VFD rows, hot cues, loops, stem toggles and the Load button.
        // The right column (pitch / nudge / VU) sizes its sliders to this
        // group's measured height so the faders run down to the Load row.
        const ImVec2 leftTop = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();
        const char* playLabel = deck.playing() ? "Pause" : "Play";
        if (ImGui::Button(playLabel, ImVec2(80.0f, 0.0f))) {
            if (!deck.playing() && atEnd) deck.requestSeek(0); // restart from end
            deck.setPlaying(!deck.playing());
        }
        ImGui::SameLine();
        // CUE (SPEC §4.5): press while playing = jump to the main cue and
        // pause; press while paused = set the cue at the current position and
        // preview it while held; release = return to the cue and pause. The
        // main cue is session-only and defaults to the track start.
        if (ImGui::Button("CUE", ImVec2(60.0f, 0.0f))) {
            // ImGui reports the click on release; the actions below use the
            // press/hold/release edges instead.
        }
        const bool cuePressed = ImGui::IsItemClicked();
        const bool cueHeld = ImGui::IsItemActive();
        if (cuePressed) {
            if (deck.playing()) {
                deck.requestSeek(mainCueFrame_);
                deck.setPlaying(false);
                cuePreviewing_ = false;
            } else {
                mainCueFrame_ = static_cast<int64_t>(deck.positionFrames());
                if (cueHeld) { // still held: preview from the fresh cue
                    deck.setPlaying(true);
                    cuePreviewing_ = true;
                }
            }
        } else if (cuePreviewing_ && !cueHeld) {
            deck.requestSeek(mainCueFrame_); // release: back to the cue
            deck.setPlaying(false);
            cuePreviewing_ = false;
        }
        // M5e: the readout rows moved below the Play/CUE line - at the
        // bigger time+pitch size the combined row grew ~60 px wider than
        // the buttons and pushed the pitch/nudge/VU column out of the
        // narrower Mix-mode deck child (VU clipped). On its own row the
        // wide readout only sets the left column's width, which the right
        // column then measures against.
        // Remaining time + pitch % (user decision 2026-10-05): total -
        // position with a CDJ-style minus sign (clamped at 0 inside
        // formatTime), pitch beside it; the bar.beat + effective BPM row
        // below appears when the track has a grid. The pitch state machine
        // runs later in this function (the nudge needs the slider item), so
        // this row shows last frame's value while dragging - a 16 ms lag,
        // imperceptible.
        const double fs = static_cast<double>(std::max(data->sampleRate, 1u));
        const double posSec = deck.positionFrames() / fs;
        const double totalSec = static_cast<double>(data->frames) / fs;
        char remCore[24], remBuf[32];
        formatTime(totalSec - posSec, remCore, sizeof(remCore));
        std::snprintf(remBuf, sizeof(remBuf), "-%s", remCore);
        char pitchBuf[32];
        std::snprintf(pitchBuf, sizeof(pitchBuf), "%+.2f%%",
                      nudging_ ? nudgeTarget_ : pitchPct_);
        const VfdSeg timeSegs[] = {
            {remBuf, true}, {"  ", false}, {pitchBuf, true}};
        vfdLine(timeSegs, 3, readoutSize_); // M5e: big time+pitch row
        if (data->bpm > 0.0f) {
            // M4d: the beat phase is aligned to the grid (first beat offset)
            // and shown as bar.beat with the effective BPM, DJ-style.
            const double beats =
                std::max(0.0, posSec - data->firstBeatOffsetMs / 1000.0) *
                static_cast<double>(data->bpm) * deck.rate() / 60.0;
            const long bar = static_cast<long>(beats / 4.0) + 1;
            const long beat = static_cast<long>(beats - std::floor(beats / 4.0) * 4.0) + 1;
            char barBeat[24], bpmN[24];
            std::snprintf(barBeat, sizeof(barBeat), "%ld.%ld", bar, beat);
            std::snprintf(bpmN, sizeof(bpmN), "%.1f",
                          static_cast<double>(data->bpm) * deck.rate());
            const VfdSeg beatSegs[] = {
                {"bar ", false}, {barBeat, true}, {" | ", false},
                {bpmN, true},    {" BPM", false}};
            vfdLine(beatSegs, 5);
        }
        // --- hot cues (SPEC §4.5, M4d): 8 slots, left-click set/trigger,
        // right-click clear; edits persist via saveCues() (meta.json / sidecar).
        if (data) {
            ImGui::Spacing();
            const double fs2 = static_cast<double>(std::max(data->sampleRate, 1u));
            for (int s = 0; s < 8; ++s) {
                ImGui::PushID(s);
                // Re-read every iteration: setHotCue() may reallocate the vector.
                const std::vector<tng::Cue>& cues = deck.track()->cues;
                const bool filled = s < static_cast<int>(cues.size());
                if (filled) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.45f, 0.70f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.55f, 0.82f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.17f, 0.38f, 0.62f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.16f, 0.19f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.22f, 0.26f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.27f, 0.32f, 1.0f));
                }
                char lbl[8];
                std::snprintf(lbl, sizeof(lbl), "%d", s + 1);
                if (ImGui::Button(lbl, ImVec2(38.0f, 0.0f))) {
                    if (filled) {
                        int64_t frame =
                            static_cast<int64_t>(cues[static_cast<size_t>(s)].positionMs * fs2 / 1000.0);
                        if (frame < 0) frame = 0;
                        if (frame >= data->frames) frame = data->frames - 1;
                        deck.requestSeek(frame); // declicked jump (SPEC §4.5)
                    } else {
                        deck.setHotCue(s, deck.positionFrames() / fs2 * 1000.0);
                        persistCues(deck, "hot cue set");
                    }
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && filled) {
                    deck.clearHotCue(s);
                    persistCues(deck, "hot cue cleared");
                }
                if (ImGui::IsItemHovered()) {
                    if (filled) {
                        char tt[96];
                        std::snprintf(tt, sizeof(tt),
                                      "hot cue %d at %.2f s - click to jump, right-click to clear",
                                      s + 1, cues[static_cast<size_t>(s)].positionMs / 1000.0);
                        ImGui::SetTooltip("%s", tt);
                    } else {
                        ImGui::SetTooltip("hot cue %d empty - click to set at the playhead", s + 1);
                    }
                }
                ImGui::PopStyleColor(3);
                ImGui::PopID();
                if (s != 7) ImGui::SameLine(0.0f, 4.0f); // M5e: Mix-mode fit
            }
        }

        // --- loop controls (SPEC §4.5, M4d): manual in/out + on/off + exit and
        // quick loops sized from the beat grid.
        if (data) {
            ImGui::Spacing();
            const int64_t pos = static_cast<int64_t>(deck.positionFrames());
            const bool hasBpm = data->bpm > 0.0f;
            const bool pairSet = deck.loopIn() >= 0 && deck.loopOut() > deck.loopIn();

            auto litButton = [](const char* label, float w, bool lit) {
                if (lit) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.45f, 0.70f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.55f, 0.82f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.17f, 0.38f, 0.62f, 1.0f));
                }
                const bool pressed = ImGui::Button(label, ImVec2(w, 0.0f));
                if (lit) ImGui::PopStyleColor(3);
                return pressed;
            };

            if (litButton("In", 34.0f, deck.loopIn() >= 0)) deck.setLoopIn(pos);
            ImGui::SameLine();
            if (litButton("Out", 38.0f, deck.loopOut() >= 0)) deck.setLoopOut(pos);
            ImGui::SameLine();
            if (!pairSet) ImGui::BeginDisabled();
            if (litButton("On", 38.0f, deck.loopActive())) {
                deck.setLoopActive(!deck.loopActive());
            }
            if (!pairSet) ImGui::EndDisabled();
            ImGui::SameLine();
            if (litButton("Exit", 44.0f, false)) deck.clearLoop();

            ImGui::SameLine();
            static const int kQuick[5] = {1, 2, 4, 8, 16};
            for (int q = 0; q < 5; ++q) {
                ImGui::PushID(100 + q);
                if (!hasBpm) ImGui::BeginDisabled();
                char qlbl[8];
                std::snprintf(qlbl, sizeof(qlbl), "%d", kQuick[q]);
                if (ImGui::Button(qlbl, ImVec2(28.0f, 0.0f)) && !deck.setQuickLoop(kQuick[q])) {
                    notice_ = "quick loop not possible here";
                    noticeErr_ = true;
                }
                if (!hasBpm) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (hasBpm) {
                        ImGui::SetTooltip("%d-beat loop from the playhead (Alt+%d)",
                                          kQuick[q], q + 1);
                    } else {
                        ImGui::SetTooltip("quick loops need a BPM in meta.json");
                    }
                }
                ImGui::PopID();
                if (q != 4) ImGui::SameLine(0.0f, 4.0f); // M5e: Mix-mode fit
            }
        }

        // --- stem toggles (colored, SPEC §6), M5e inside the left column ----
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

        // --- load (M5e): native zip picker instead of the old path input
        // row; dropping a folder/zip on the window stays the main action.
        ImGui::Spacing();
        if (ImGui::Button("Load...", ImVec2(110.0f, 0.0f))) {
            loadResult_ = pickZipDialog(nativeHwnd_);
            if (!loadResult_.empty()) loadPath = loadResult_.c_str();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("pick a track .zip (or drop one anywhere in the window)");
        }

        ImGui::EndGroup();

        // Measure the left column and size the faders to match so the
        // pitch fader runs down to where the Load button sits.
        const float leftH = ImGui::GetCursorScreenPos().y - leftTop.y;
        const ImGuiStyle& style = ImGui::GetStyle();
        const float labelRow = ImGui::GetTextLineHeight() + style.ItemSpacing.y;
        const float btnRow = ImGui::GetFrameHeight() + style.ItemSpacing.y;
        const float sliderH = std::max(68.0f, leftH - labelRow - btnRow);
        const float vuH = std::max(68.0f, leftH - labelRow);

        // --- vertical pitch fader (SPEC §6): ------------------------------
        // M5c: direction is a persisted setting (SPEC §4.3). Reversed
        // (default, DJ gear): top = slow, bottom = fast. Straight: top =
        // fast. The widget always edits the on-screen value; pitchPct_ is
        // mapped through the direction. The % readout sits beside the
        // remaining time on its own row (M5e moved both below Play/CUE).
        // M5e: plain slider - the momentary bend moved to the nudge slider
        // next to it (Shift+click is gone, see below).
        ImGui::SameLine(0.0f, 8.0f); // M5e: 8 px keeps Mix mode under 468 px
        ImGui::BeginGroup();
        ImGui::TextDisabled("pitch");
        float shown = pitchReversed_ ? -pitchPct_ : pitchPct_;
        ImGui::VSliderFloat("##pitch", ImVec2(34.0f, sliderH), &shown, -pitchRange_,
                            pitchRange_, "", ImGuiSliderFlags_AlwaysClamp);

        if (nudging_) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // Release: snap back to the base pitch (the 15 ms rate glide
                // smooths both the bend and the return).
                pitchPct_ = nudgeBase_;
                deck.setRate(1.0 + pitchPct_ / 100.0);
                nudging_ = false;
            } else {
                deck.setRate(1.0 + nudgeTarget_ / 100.0); // handle stays at base
            }
        } else if (ImGui::IsItemActive()) {
            pitchPct_ = pitchReversed_ ? -shown : shown; // drag: map back through direction
            deck.setRate(1.0 + static_cast<double>(pitchPct_) / 100.0);
        } else {
            pitchPct_ = static_cast<float>((deck.rate() - 1.0) * 100.0); // idle: mirror
        }

        // Range toggle (SPEC §4.3, user decision: 2-state 10/16 for v1).
        // M5e: 34 px wide to match the slider (was 44).
        char rangeLbl[16];
        std::snprintf(rangeLbl, sizeof(rangeLbl), "%.0f%%", pitchRange_);
        if (ImGui::Button(rangeLbl, ImVec2(34.0f, 0.0f))) {
            setPitchRange(deck, (pitchRange_ == 10.0f) ? 16.0f : 10.0f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("pitch range - click to toggle (10%% default, 16%%)");
        }
        ImGui::EndGroup();

        // --- nudge slider (M5e): momentary pitch bend next to the pitch
        // fader, replacing Shift+click. Click above/below center and hold =
        // bend toward that side (magnitude = distance, min 0.5%), release
        // snaps back via the same nudging_/nudgeTarget_ state machine in
        // the pitch block above. M5g: the click is a delta APPENDED to the
        // pitch the deck is playing at (center = no change, top/bottom =
        // +/-full range), never an absolute value from 0 % - at -8 % pitch
        // the slow side must slow down, not jump toward zero. Direction
        // follows the fader setting: reversed (default) = down is faster.
        // M5f/M5g: the handle shows the applied bend while held (fixed at
        // the clicked offset) and snaps to center on release - drawn
        // manually, it never trails the mouse.
        // No label row (M5e plan: right column = pitch label + sliders)
        // - a "nudge" caption would widen the group past what the
        // Mix-mode deck child can hold; the tooltip teaches it instead.
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(1.0f, labelRow)); // reserve the label row, align tops
        const ImVec2 npos = ImGui::GetCursorScreenPos();
        const ImVec2 nsize(34.0f, sliderH);
        ImDrawList* ndl = ImGui::GetWindowDrawList();
        ndl->AddRectFilled(npos, ImVec2(npos.x + nsize.x, npos.y + nsize.y),
                           IM_COL32(14, 14, 18, 255));
        const float nMid = npos.y + nsize.y * 0.5f;
        ndl->AddLine(ImVec2(npos.x + 1.0f, nMid), ImVec2(npos.x + nsize.x - 1.0f, nMid),
                     IM_COL32(96, 244, 224, 120));
        // M5g: handle offset = the applied bend (target - base), so center
        // = the pitch the deck plays at and the strip reads +/- the bend;
        // the press math below inverts this exact formula, clamped to stay
        // inside.
        {
            const float k = pitchReversed_ ? 1.0f : -1.0f;
            const float shown = nudging_ ? (nudgeTarget_ - nudgeBase_) : 0.0f;
            float hy = nMid;
            if (pitchRange_ > 0.0f)
                hy = npos.y + nsize.y * (shown / (2.0f * k * pitchRange_) + 0.5f);
            hy = std::max(npos.y + 6.0f, std::min(npos.y + nsize.y - 6.0f, hy));
            ndl->AddRectFilled(ImVec2(npos.x + 5.0f, hy - 5.0f),
                               ImVec2(npos.x + nsize.x - 5.0f, hy + 5.0f),
                               IM_COL32(170, 178, 186, 255));
        }
        ndl->AddRect(npos, ImVec2(npos.x + nsize.x, npos.y + nsize.y),
                     IM_COL32(60, 60, 70, 255));
        ImGui::Dummy(nsize);
        const bool nHover = ImGui::IsItemHovered();
        if (nHover) {
            ImGui::SetTooltip(
                "nudge: click above/below center and hold = bend the current "
                "pitch by that offset, release snaps back");
        }
        if (nHover && !nudging_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            nudging_ = true;
            nudgeBase_ = pitchPct_; // the pitch playing at the moment of press
            const float norm = (ImGui::GetIO().MousePos.y - npos.y) / nsize.y;
            const float side = (norm >= 0.5f) ? 1.0f : -1.0f;
            // M5g: norm -> bend DELTA (center = no change), not absolute
            // pitch: reversed below = bend +pitch (faster), straight below
            // = bend -pitch, i.e. the bend direction follows the fader.
            const float k = pitchReversed_ ? 1.0f : -1.0f;
            float delta = k * pitchRange_ * (2.0f * norm - 1.0f);
            if (std::fabs(delta) < 0.5f) {
                delta = 0.5f * side * k;
            }
            const float target = nudgeBase_ + delta;
            nudgeTarget_ = std::max(-pitchRange_, std::min(pitchRange_, target));
            deck.setRate(1.0 + nudgeTarget_ / 100.0); // bend now, this frame
        }
        // Row filler: matches the pitch group's range-button row below.
        ImGui::Dummy(ImVec2(34.0f, ImGui::GetFrameHeight()));
        ImGui::EndGroup();

        // --- per-deck VU meter (M5g: Single mode only - in Mix mode the
        // meters moved to the mixer column, beside the A/B line faders) ---
        // Peak of the deck's own output (pre line fader/crossfader), peak-hold
        // with a fast fall so the bar is readable at 60 fps.
        if (showVU) {
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::BeginGroup();
            ImGui::TextDisabled("VU");
            const float vuPeak = std::min(1.0f, deck.blockPeak());
            vuLevel_ = std::max(vuPeak, vuLevel_ * 0.88f);
            const ImVec2 vuPos = ImGui::GetCursorScreenPos();
            const float vuW = 22.0f;
            drawVuBar(ImGui::GetWindowDrawList(), vuPos.x, vuPos.y, vuW, vuH,
                      vuLevel_);
            ImGui::Dummy(ImVec2(vuW, vuH));
            ImGui::EndGroup();
        }
    } else {
        // No track: the placeholder box is drawn above; offer the Load
        // button on its own (M5e - no right column without data).
        ImGui::Spacing();
        if (ImGui::Button("Load...", ImVec2(110.0f, 0.0f))) {
            loadResult_ = pickZipDialog(nativeHwnd_);
            if (!loadResult_.empty()) loadPath = loadResult_.c_str();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("pick a track .zip (or drop one anywhere in the window)");
        }
    }

    return loadPath;
}

bool DeckView::takeNotice(std::string& out, bool& err) {
    if (notice_.empty()) return false;
    out = notice_;
    err = noticeErr_;
    notice_.clear();
    return true;
}

void DeckView::cancelNudge(tng::Deck& deck) {
    if (!nudging_) return;
    nudging_ = false;
    pitchPct_ = nudgeBase_;
    deck.setRate(1.0 + pitchPct_ / 100.0);
}

void DeckView::persistCues(const tng::Deck& deck, const char* what) {
    if (trackPath_.empty() || deck.track() == nullptr) {
        notice_ = std::string(what) + ": no track path to save to";
        noticeErr_ = true;
        return;
    }
    std::string err;
    if (tng::saveCues(trackPath_, deck.track()->cues, &err)) {
        notice_ = std::string(what) + " - saved";
        noticeErr_ = false;
    } else {
        notice_ = std::string(what) + " - save failed: " + err;
        noticeErr_ = true;
    }
}

} // namespace tui
