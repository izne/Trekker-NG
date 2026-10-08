#include "deck_view.h"

#include "imgui.h"
#include "track_loader.h" // saveCues after hot-cue edits (M4d)

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

// --- VFD readout (M4display) ------------------------------------------------
// One dark inset box with mixed-font segments: DSEG7 cyan numbers (the
// default font is merged into vfdFont, so '+', '%', '/' fall back to it
// automatically) next to plain-font words like "beat"/"BPM".
struct VfdSeg {
    const char* text;
    bool dseg;
};

void vfdLine(const VfdSeg* segs, int count) {
    static const ImU32 kInset = IM_COL32(8, 12, 14, 255);
    static const ImU32 kCyan = IM_COL32(96, 244, 224, 255);
    static const ImU32 kWord = IM_COL32(170, 178, 186, 255);
    const float padX = 4.0f, padY = 2.0f;
    if (count > 8) count = 8;
    float widths[8], heights[8];
    float total = 0.0f, maxH = 0.0f;
    for (int i = 0; i < count; ++i) {
        ImVec2 ts;
        if (segs[i].dseg && vfdFont) {
            ImGui::PushFont(vfdFont, kVfdFontSize);
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
            dl->AddText(vfdFont, kVfdFontSize, ImVec2(x, y), kCyan, segs[i].text);
        } else {
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y),
                        kWord, segs[i].text);
        }
        x += widths[i];
    }
    ImGui::Dummy(box);
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
        // and its color is dimmed).
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

const char* DeckView::draw(tng::Deck& deck, bool active) {
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
        // Left block: buttons + remaining time and pitch % on the first
        // line, bar.beat | BPM under the buttons, hot cues and the loop row
        // after that. Keeping the long text off the first line leaves the
        // pitch fader and the VU meter inside the deck's column width, and
        // the cue/loop rows inside this group use the dead space under the
        // bar line - the deck child clips anything that flows after the
        // fader/VU line (M4d lesson).
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
        ImGui::SameLine();
        // Remaining time + pitch % on the first row (user decision
        // 2026-10-05): total - position with a CDJ-style minus sign
        // (clamped at 0 inside formatTime), pitch in the freed space; the
        // bar.beat + effective BPM row below appears when the track has a
        // grid. The pitch state machine runs later in this function (the
        // nudge needs the slider item), so this row shows last frame's
        // value while dragging - a 16 ms lag, imperceptible.
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
        vfdLine(timeSegs, 3);
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
                if (s != 7) ImGui::SameLine();
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
                if (q != 4) ImGui::SameLine();
            }
        }

        ImGui::EndGroup();

        // --- vertical pitch fader (SPEC §6): ------------------------------
        // M5c: direction is a persisted setting (SPEC §4.3). Reversed
        // (default, DJ gear): top = slow, bottom = fast. Straight: top =
        // fast. The widget always edits the on-screen value; pitchPct_ is
        // mapped through the direction. The % readout sits on the transport
        // line next to the remaining time (user decision 2026-10-05).
        // M4d: Shift+click nudge (jog-style bend) + the range toggle button.
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::BeginGroup();
        ImGui::TextDisabled("pitch");
        float shown = pitchReversed_ ? -pitchPct_ : pitchPct_;
        ImGuiIO& io = ImGui::GetIO();
        const bool armed = io.KeyShift || nudging_; // nudge armed: slider inert
        if (armed) ImGui::BeginDisabled();
        ImGui::VSliderFloat("##pitch", ImVec2(34.0f, 68.0f), &shown, -pitchRange_,
                            pitchRange_, "", ImGuiSliderFlags_AlwaysClamp);
        const bool faderHover =
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (armed) ImGui::EndDisabled();

        if (io.KeyShift && faderHover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // Nudge: bend toward the value under the cursor (below the
            // handle = slider low end = +pitch/faster when reversed,
            // -pitch/slower when straight), magnitude = click distance,
            // minimum 0.5% on that side.
            nudging_ = true;
            nudgeBase_ = pitchPct_;
            const ImVec2 r0 = ImGui::GetItemRectMin();
            const ImVec2 r1 = ImGui::GetItemRectMax();
            const float norm = (io.MousePos.y - r0.y) / (r1.y - r0.y);
            const float shownBase = pitchReversed_ ? -nudgeBase_ : nudgeBase_;
            const float handleNorm = (pitchRange_ - shownBase) / (2.0f * pitchRange_);
            const float side = (norm >= handleNorm) ? 1.0f : -1.0f;
            // norm -> pitchPct: reversed below = +pitch, straight below = -pitch
            const float k = pitchReversed_ ? 1.0f : -1.0f;
            float target = k * pitchRange_ * (2.0f * norm - 1.0f);
            if (std::fabs(target - nudgeBase_) < 0.5f) target = nudgeBase_ + 0.5f * side * k;
            nudgeTarget_ = std::max(-pitchRange_, std::min(pitchRange_, target));
        }

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
        char rangeLbl[16];
        std::snprintf(rangeLbl, sizeof(rangeLbl), "%.0f%%", pitchRange_);
        if (ImGui::Button(rangeLbl, ImVec2(44.0f, 0.0f))) {
            setPitchRange(deck, (pitchRange_ == 10.0f) ? 16.0f : 10.0f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("pitch range - click to toggle (10%% default, 16%%)");
        }
        ImGui::EndGroup();

        // --- per-deck VU meter (not in SPEC; small bonus) -------------------
        // Peak of the deck's own output (pre line fader/crossfader), peak-hold
        // with a fast fall so the bar is readable at 60 fps. Clamped: four
        // hot stems can sum past 1.0, the bar just stays full.
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::BeginGroup();
        ImGui::TextDisabled("VU");
        const float vuPeak = std::min(1.0f, deck.blockPeak());
        vuLevel_ = std::max(vuPeak, vuLevel_ * 0.88f);
        const ImVec2 vuPos = ImGui::GetCursorScreenPos();
        const float vuW = 22.0f;
        const float vuH = 68.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(vuPos, ImVec2(vuPos.x + vuW, vuPos.y + vuH),
                          IM_COL32(14, 14, 18, 255));
        const float fill = vuLevel_ * (vuH - 2.0f);
        const ImU32 vuColor = vuLevel_ > 0.9f   ? IM_COL32(255, 70, 60, 255)
                              : vuLevel_ > 0.7f ? IM_COL32(255, 200, 60, 255)
                                                : IM_COL32(90, 210, 120, 255);
        dl->AddRectFilled(ImVec2(vuPos.x + 1.0f, vuPos.y + vuH - 1.0f - fill),
                          ImVec2(vuPos.x + vuW - 1.0f, vuPos.y + vuH - 1.0f), vuColor);
        dl->AddRect(vuPos, ImVec2(vuPos.x + vuW, vuPos.y + vuH), IM_COL32(60, 60, 70, 255));
        ImGui::Dummy(ImVec2(vuW, vuH));
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
