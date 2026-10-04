#pragma once

#include "deck.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tng {

struct LoadResult {
    std::unique_ptr<DeckData> data; // null on failure
    std::string error;              // human-readable reason

    bool ok() const { return data != nullptr; }
};

// Loads a Trekker-NG track: either a folder or a .zip containing meta.json and
// 1..4 stem audio files (SPEC §5). All stems are fully decoded to RAM as
// stereo float32 and padded to equal length. Missing stems stay silent.
//
// Cue points (SPEC §4.5): the "cues" array of meta.json seeds DeckData::cues;
// for .zip tracks a sidecar "<archive>.cues.json" next to the archive
// overrides it (the zip itself stays read-only). The string helpers below are
// pure so the test suite can stay in-memory (tests/testutil.h).
LoadResult loadTrack(const std::filesystem::path& path);

// Fills `out` from the "cues" array of a meta.json document. Returns false
// only when `metaText` is not valid JSON (error then set); an absent or
// non-array "cues" key leaves `out` untouched. Non-object entries are
// skipped, wrong field types ignored, negative positions clamped to 0.
bool parseCuesFromMeta(const std::string& metaText, std::vector<Cue>& out,
                       std::string* error);

// Returns `metaText` with its "cues" array replaced by `cues`. All other
// fields (including unknown ones, SPEC §5) survive; key order may change.
// False when `metaText` is not valid JSON.
bool setCuesInMeta(const std::string& metaText, const std::vector<Cue>& cues,
                   std::string& outMetaText, std::string* error);

// Persists cue edits: a folder track gets its meta.json rewritten (temp file
// + rename, unknown fields preserved); a .zip gets a sidecar
// "<archive>.cues.json" - the archive itself is never modified.
bool saveCues(const std::filesystem::path& trackPath, const std::vector<Cue>& cues,
              std::string* error);

} // namespace tng
